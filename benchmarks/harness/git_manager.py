"""
Git Worktree and Target Version Manager.
Extracts and isolates comparison targets (Original Fork, Last Commit, Current Changes)
into separate build worktrees without modifying the active working copy.
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path
from typing import Dict, Optional, Tuple


class GitManager:
    """Manages git checkouts and isolated worktrees for benchmark comparison."""

    def __init__(self, repo_root: Optional[Path] = None):
        if repo_root is None:
            self.repo_root = Path(__file__).resolve().parent.parent.parent
        else:
            self.repo_root = Path(repo_root)

        self.worktrees_dir = self.repo_root / "benchmarks" / ".worktrees"
        self.worktrees_dir.mkdir(parents=True, exist_ok=True)

    def _run_git(self, args: list, cwd: Optional[Path] = None) -> Tuple[int, str, str]:
        work_dir = str(cwd or self.repo_root)
        proc = subprocess.run(
            ["git"] + args,
            cwd=work_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace"
        )
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()

    def get_commit_hash(self, ref: str = "HEAD") -> str:
        """Resolves a git reference to full commit SHA."""
        rc, out, _ = self._run_git(["rev-parse", ref])
        if rc == 0:
            return out
        return ref

    def get_commit_summary(self, ref: str = "HEAD") -> str:
        """Retrieves single-line commit summary."""
        rc, out, _ = self._run_git(["log", "-n", "1", "--format=%h %s (%an, %cd)", "--date=short", ref])
        if rc == 0:
            return out
        return ref

    def has_uncommitted_changes(self) -> bool:
        """Checks if working tree has modified/staged/untracked files."""
        rc, out, _ = self._run_git(["status", "--porcelain"])
        return rc == 0 and len(out) > 0

    def resolve_target_ref(self, target_name: str, config: Dict) -> Tuple[str, str]:
        """
        Resolves target name into (git_ref, description).
        Handles 'original-fork', 'last-commit', 'current'.
        """
        target_cfg = config.get("targets_config", {}).get(target_name, {})
        git_ref = target_cfg.get("git_ref", target_name)
        desc = target_cfg.get("description", target_name)

        if target_name == "original-fork":
            # Check if upstream/master exists, otherwise fallback to efd55a1
            rc, _, _ = self._run_git(["rev-parse", "--verify", "upstream/master"])
            if rc == 0:
                git_ref = "upstream/master"
            else:
                git_ref = target_cfg.get("fallback_ref", "efd55a1")

        elif target_name == "last-commit":
            git_ref = "HEAD~1"

        elif target_name == "current":
            git_ref = "WORKING_TREE"

        return git_ref, desc

    def prepare_target_source(self, target_name: str, git_ref: str) -> Path:
        """
        Prepares an isolated directory containing the exact source tree for the target.
        For 'WORKING_TREE', uses the current repo root (or an rsync/copy if desired).
        For git refs, creates a git worktree.
        """
        if git_ref == "WORKING_TREE":
            return self.repo_root

        target_dir = self.worktrees_dir / target_name

        # If worktree already exists, verify its commit
        if target_dir.exists() and (target_dir / ".git").exists():
            resolved_sha = self.get_commit_hash(git_ref)
            current_sha = self.get_commit_hash(f"{target_dir}:HEAD") if (target_dir / ".git").exists() else ""
            
            # Prune and recreate if commit differs
            print(f"[*] Updating worktree '{target_name}' to {git_ref} ({resolved_sha[:8]})...")
            self._run_git(["worktree", "remove", "--force", str(target_dir)])
            if target_dir.exists():
                shutil.rmtree(target_dir, ignore_errors=True)

        print(f"[*] Creating isolated git worktree for '{target_name}' at {git_ref}...")
        rc, out, err = self._run_git(["worktree", "add", "--force", "--detach", str(target_dir), git_ref])
        if rc != 0:
            # Fallback: clone local repo to destination
            print(f"[-] worktree add failed ({err}), falling back to direct checkout clone...")
            if target_dir.exists():
                shutil.rmtree(target_dir, ignore_errors=True)
            self._run_git(["clone", "--shared", "--no-checkout", str(self.repo_root), str(target_dir)])
            self._run_git(["checkout", git_ref], cwd=target_dir)

        # Copy local pre-existing dependencies from root workspace (WTL and ATL/MFC)
        for dep_name in ["WTL-sf", "atlmfc"]:
            root_dep = self.repo_root / "deps" / dep_name
            target_dep = target_dir / "deps" / dep_name
            if root_dep.exists():
                target_dep.parent.mkdir(parents=True, exist_ok=True)
                shutil.copytree(root_dep, target_dep, dirs_exist_ok=True)

        # For legacy MSBuild targets without CMakeLists.txt: inject Directory.Build.props
        if not (target_dir / "CMakeLists.txt").exists():
            props_content = """<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <PreprocessorDefinitions>_ATL_NO_DEFAULT_LIBS;USE_ATL_THUNK1;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <AdditionalIncludeDirectories>$(MSBuildThisFileDirectory)deps\\atlmfc\\atlmfc\\include;$(MSBuildThisFileDirectory)deps\\WTL-sf\\Include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Link>
      <AdditionalLibraryDirectories>$(MSBuildThisFileDirectory)src\\WICLoader\\JPEGView\\bin\\x64\\Release;$(MSBuildThisFileDirectory)src\\JPEGView\\bin\\x64\\Release;$(MSBuildThisFileDirectory)bin\\x64\\Release;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>shlwapi.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
</Project>
"""
            with open(target_dir / "Directory.Build.props", "w", encoding="utf-8") as f:
                f.write(props_content)

            # Also ensure stdafx.cpp has ATL thunks
            root_stdafx_cpp = self.repo_root / "src" / "JPEGView" / "stdafx.cpp"
            target_stdafx_cpp = target_dir / "src" / "JPEGView" / "stdafx.cpp"
            if root_stdafx_cpp.exists() and target_stdafx_cpp.exists():
                shutil.copy2(root_stdafx_cpp, target_stdafx_cpp)

        return target_dir

    def cleanup_worktrees(self):
        """Removes temporary worktrees."""
        if self.worktrees_dir.exists():
            self._run_git(["worktree", "prune"])
            shutil.rmtree(self.worktrees_dir, ignore_errors=True)


if __name__ == "__main__":
    gm = GitManager()
    print("Repo Root:", gm.repo_root)
    print("HEAD:", gm.get_commit_summary("HEAD"))
    print("Has uncommitted:", gm.has_uncommitted_changes())
