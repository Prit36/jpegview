"""
Git Repository & Worktree Isolation Manager for Benchmarks.
Creates and manages isolated worktree environments for target comparisons without disturbing dirty working trees.
Modern Python 3.12+ implementation.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from typing import Any


class GitManager:
    """Manages git branches, commits, worktrees, and working tree snapshots."""

    def __init__(self, repo_root: Path | str | None = None) -> None:
        if repo_root is None:
            self.repo_root = Path(__file__).resolve().parent.parent.parent
        else:
            self.repo_root = Path(repo_root)

        self.worktrees_dir = self.repo_root / "benchmarks" / ".worktrees"
        self.worktrees_dir.mkdir(parents=True, exist_ok=True)

    def _run_git(self, args: list[str], cwd: Path | None = None) -> tuple[int, str, str]:
        cmd = ["git"] + args
        work_dir = str(cwd if cwd is not None else self.repo_root)
        proc = subprocess.run(cmd, cwd=work_dir, capture_output=True, text=True)
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()

    def get_commit_hash(self, git_ref: str = "HEAD") -> str:
        rc, out, _ = self._run_git(["rev-parse", git_ref])
        return out if rc == 0 else "unknown"

    def get_commit_summary(self, git_ref: str = "HEAD") -> str:
        rc, out, _ = self._run_git(["log", "-1", "--format=%h - %s (%cr)", git_ref])
        return out if rc == 0 else git_ref

    def has_uncommitted_changes(self) -> bool:
        rc, out, _ = self._run_git(["status", "--porcelain"])
        return rc == 0 and len(out.strip()) > 0

    def resolve_target_ref(self, target_name: str, config: dict[str, Any]) -> tuple[str, str]:
        """Resolves target names to specific git refs and human descriptions."""
        target_defs = config.get("target_definitions", {})

        match target_name:
            case "original-fork":
                git_ref = target_defs.get("original-fork", {}).get("git_ref", "upstream/master")
                rc, _, _ = self._run_git(["rev-parse", "--verify", git_ref])
                if rc != 0:
                    git_ref = "efd55a1"
                desc = "Upstream master fork baseline before modern optimizations"

            case "last-commit":
                git_ref = target_defs.get("last-commit", {}).get("git_ref", "HEAD~1")
                desc = "Previous git commit in current branch history"

            case "current":
                git_ref = "WORKING_TREE"
                desc = "Current working copy with uncommitted changes"

            case _ if target_name in target_defs:
                t_info = target_defs[target_name]
                git_ref = t_info.get("git_ref", target_name)
                desc = t_info.get("description", f"Target ref: {git_ref}")

            case _:
                git_ref = target_name
                desc = f"Custom git target ref: {target_name}"

        return git_ref, desc

    def prepare_target_source(self, target_name: str, git_ref: str) -> Path:
        """
        Prepares an isolated directory containing the exact source tree for the target.
        """
        if git_ref == "WORKING_TREE":
            return self.repo_root

        target_dir = self.worktrees_dir / target_name

        if target_dir.exists() and (target_dir / ".git").exists():
            resolved_sha = self.get_commit_hash(git_ref)
            print(f"[*] Updating worktree '{target_name}' to {git_ref} ({resolved_sha[:8]})...")
            self._run_git(["worktree", "remove", "--force", str(target_dir)])
            if target_dir.exists():
                shutil.rmtree(target_dir, ignore_errors=True)

        print(f"[*] Creating isolated git worktree for '{target_name}' at {git_ref}...")
        rc, out, err = self._run_git(["worktree", "add", "--force", "--detach", str(target_dir), git_ref])
        if rc != 0:
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
            (target_dir / "Directory.Build.props").write_text(props_content, encoding="utf-8")

            # Also ensure stdafx.cpp has ATL thunks
            root_stdafx_cpp = self.repo_root / "src" / "JPEGView" / "stdafx.cpp"
            target_stdafx_cpp = target_dir / "src" / "JPEGView" / "stdafx.cpp"
            if root_stdafx_cpp.exists() and target_stdafx_cpp.exists():
                shutil.copy2(root_stdafx_cpp, target_stdafx_cpp)

        return target_dir

    def cleanup_worktrees(self) -> None:
        """Removes temporary worktrees."""
        if self.worktrees_dir.exists():
            self._run_git(["worktree", "prune"])
            shutil.rmtree(self.worktrees_dir, ignore_errors=True)
