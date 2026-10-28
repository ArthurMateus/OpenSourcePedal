"""Merge PRs into main one at a time, bottom of each stack first (owner-approved merge train, 2026-10-27).

Usage, from a clean checkout of this repo: python tools/merge-train/merge_train.py 133 134 61 135 136 137
Update PARENT below for new stacks. See docs/state/HANDOFF/owner-session.md.

For each PR: retarget to main, merge origin/main into its head (keep-both for additive conflicts in
registry files), push, wait for the required checks, squash-merge, retarget the PRs based on its
branch to main, then delete its branch. Stops on anything it cannot resolve safely.
"""
import json, os, re, subprocess, sys, time
from test_registry_migration import resolve_test_registry_conflict

REPO = "ArthurMateus/OpenSourcePedal"
REQUIRED = {
    "guard", "windows-msvc (Release, /WX)", "clang-format", "halfband-check",
    "windows-msvc-debug (RT checker)", "pluginval (v1.0.4, strictness 9)",
}
# #115 removes shared source/benchmark append lists. Conflicts in CMake, benchmark bodies, or budget fragments
# now need an explicit resolution: blindly keeping both can duplicate a target, benchmark, or JSON key.
KEEP_BOTH_OK = re.compile(r"^(THIRD_PARTY\.md|assets/LICENSES\.md)$")
TRAILER = ""
os.environ["GIT_LFS_SKIP_SMUDGE"] = "1"


def log(*a):
    line = time.strftime("%H:%M:%S") + " " + " ".join(map(str,a))
    print(line, flush=True)
    with open("C:/osptrain-control/train.log", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def run(*cmd, check=True):
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and p.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} -> {p.returncode}\n{p.stdout}\n{p.stderr}")
    return p


def pr(n, fields):
    for attempt in range(6):  # ride out network blips (TLS handshake timeouts)
        try:
            return json.loads(run("gh", "pr", "view", str(n), "--json", fields).stdout)
        except RuntimeError:
            if attempt == 5:
                raise
            time.sleep(20)


def balance(lines):
    s = re.sub(r'"(\\.|[^"\\])*"', '""', "".join(re.sub(r"//.*", "", l) for l in lines))
    return sum(s.count(c) for c in "({[") - sum(s.count(c) for c in ")}]")


def resolve_keep_both(path):
    lines = open(path, encoding="utf-8").read().replace("\r\n", "\n").splitlines(keepends=True)
    out, i = [], 0
    while i < len(lines):
        if not lines[i].startswith("<<<<<<< "):
            out.append(lines[i])
            i += 1
            continue
        j = next(k for k in range(i, len(lines)) if lines[k] == "=======\n")
        e = next(k for k in range(j, len(lines)) if lines[k].startswith(">>>>>>> "))
        ours, theirs, after = lines[i + 1:j], lines[j + 1:e], lines[e + 1:]
        if ours == theirs or not "".join(theirs).strip():
            out += ours
        elif not "".join(ours).strip():
            out += theirs
        else:
            # Two entries added at the same spot: git keeps their shared closing lines ("})});", "},")
            # after the conflict. Repeat just enough of them that each side's brackets balance.
            for k in range(0, 5):
                tail = after[:k]
                if balance(ours + tail) == 0 and balance(theirs + tail) == 0:
                    if k and k < len(after) and not after[k].strip():
                        tail = after[:k + 1]
                    out += ours + tail + theirs
                    break
            else:
                return False
        i = e + 1
    new = "".join(out)
    if "<<<<<<<" in new or ">>>>>>>" in new:
        return False
    if path.endswith(".json"):
        try:
            json.loads(new)
        except ValueError:
            return False
    open(path, "w", encoding="utf-8", newline="").write(new)
    return True


# Stacked PR -> the PR it sits on. main only has the parent as a squash commit, so first merge the parent's
# final head (shared history: only registry conflicts), then main.
PARENT = {133: 132, 134: 133, 136: 135, 137: 136, 140: 139, 149: 142, 150: 149, 151: 150, 152: 151, 153: 152, 154: 153, 155: 154, 156: 155}


def merge_ref(head, ref, label):
    p = run("git", "merge", "--no-ff", "--no-commit", ref, check=False)
    log(f"  merge {label} into {head}: rc {p.returncode}, {p.stdout.strip().splitlines()[-1:]}")
    if "Already up to date" in p.stdout:
        return False
    conflicted = [f for f in run("git", "diff", "--name-only", "--diff-filter=U").stdout.split() if f]
    for f in conflicted:
        if not (resolve_test_registry_conflict(f) or (KEEP_BOTH_OK.search(f) and resolve_keep_both(f))):
            run("git", "merge", "--abort", check=False)
            raise RuntimeError(f"{head}: conflict in {f} needs a human (conflicts: {conflicted})")
        if f.endswith((".cpp", ".h")):
            run("clang-format", "-i", f)
        if f.endswith((".cpp", ".h")) and run("clang-format", "--dry-run", "--Werror", f, check=False).returncode:
            run("git", "merge", "--abort", check=False)
            raise RuntimeError(f"{head}: keep-both resolution of {f} is not clang-format clean, needs a human")
        run("git", "add", f)
    msg = f"Merge {label} into {head} (merge train)"
    if conflicted:
        msg += "\n\nKept both sides of the additive conflicts in: " + ", ".join(conflicted)
    run("git", "commit", "-q", "-m", msg + TRAILER)
    return True


def sync_with_main(head, n=None):
    run("git", "fetch", "-q", "origin")
    run("git", "checkout", "-q", "-B", head, f"origin/{head}")
    changed = False
    if n in PARENT:
        parent_head = pr(PARENT[n], "headRefOid")["headRefOid"]
        run("git", "fetch", "-q", "origin", parent_head)
        changed |= merge_ref(head, parent_head, f"#{PARENT[n]}'s final head")
        if run("git", "diff", "--quiet", parent_head, "origin/main", check=False).returncode == 0:
            # main's tree is exactly the parent's final head, which this branch now contains.
            if run("git", "merge-base", "--is-ancestor", "origin/main", "HEAD", check=False).returncode:
                run("git", "merge", "-q", "-s", "ours", "origin/main", "-m",
                    f"Merge origin/main into {head} (merge train)\n\nmain's tree equals #{PARENT[n]}'s final "
                    "head, which this branch already contains." + TRAILER)
                changed = True
            if changed:
                run("git", "push", "-q", "origin", f"HEAD:{head}")
            return changed
    changed |= merge_ref(head, "origin/main", "origin/main")
    if changed:
        run("git", "push", "-q", "origin", f"HEAD:{head}")
    return changed


def wait_checks(n, sha, timeout=90 * 60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = pr(n, "headRefOid,statusCheckRollup")
        if d["headRefOid"] != sha and time.time() - t0 < 120:
            time.sleep(10)  # GitHub reports the pre-push head for a little while
            continue
        if d["headRefOid"] != sha:
            raise RuntimeError(f"#{n}: head moved to {d['headRefOid']} while waiting (Auto-fix push?)")
        got = {c.get("name") or c.get("context"): (c.get("conclusion") or c.get("state") or c.get("status"))
               for c in d["statusCheckRollup"]}
        # Wait for every job (Debug RT checker, pluginval too), not just the three the branch rule requires.
        bad = [k for k, v in got.items() if v in ("FAILURE", "CANCELLED", "TIMED_OUT", "ERROR", "ACTION_REQUIRED")]
        if bad:
            raise RuntimeError(f"#{n}: check failed: {bad}")
        if (REQUIRED <= set(got) and all(got[name] == "SUCCESS" for name in REQUIRED)
                and all(v in ("SUCCESS", "SKIPPED", "NEUTRAL") for v in got.values())):
            return
        time.sleep(30)
    raise RuntimeError(f"#{n}: checks did not finish in {timeout // 60} min")


def merge_one(n):
    d = pr(n, "state,headRefName,baseRefName,headRefOid,mergeStateStatus")
    if d["state"] == "MERGED":
        log(f"#{n} already merged")
        return
    if d["state"] != "OPEN":
        raise RuntimeError(f"#{n} is {d['state']}")
    head = d["headRefName"]
    if d["baseRefName"] != "main":
        run("gh", "api", "-X", "PATCH", f"repos/{REPO}/pulls/{n}", "-f", "base=main")
        log(f"#{n} retargeted {d['baseRefName']} -> main")
    if sync_with_main(head, n):
        log(f"#{n} synced with main, waiting for CI")
    sha = run("git", "rev-parse", f"origin/{head}").stdout.strip()
    wait_checks(n, sha)
    for _ in range(12):
        st = pr(n, "mergeStateStatus,mergeable")
        if st["mergeable"] != "UNKNOWN":
            break
        time.sleep(5)
    if st["mergeable"] == "CONFLICTING":
        raise RuntimeError(f"#{n}: conflicts with main after sync")
    subj = run("gh", "pr", "view", str(n), "--json", "title", "--jq", ".title").stdout.strip()
    run("gh", "pr", "merge", str(n), "--squash", "--subject", f"{subj} (#{n})", "--body", TRAILER.strip())
    log(f"#{n} merged")
    children = json.loads(run("gh", "pr", "list", "--state", "open", "--base", head, "--json", "number").stdout)
    for c in children:
        # GitHub may already have retargeted it (branch auto-delete); that PATCH then fails, which is fine.
        p = run("gh", "api", "-X", "PATCH", f"repos/{REPO}/pulls/{c['number']}", "-f", "base=main", check=False)
        log(f"  child #{c['number']} -> main ({'retargeted' if p.returncode == 0 else 'already'})")
    run("git", "push", "-q", "origin", "--delete", head, check=False)


if __name__ == "__main__":
    for n in [int(x) for x in sys.argv[1:]]:
        try:
            merge_one(n)
        except Exception as e:  # stop the train, report
            log(f"STOP at #{n}: {e}")
            sys.exit(1)
    log("train done")
