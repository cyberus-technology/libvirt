from gitlint.options import ListOption
from gitlint.rules import CommitRule, RuleViolation


class CommitFilter(CommitRule):
    """Reject commits that mix filtered and non-filtered paths."""

    name = "commit-must-not-mix-filtered-and-unfiltered-paths"
    id = "UC-scope"
    options_spec = [
        ListOption(
            "filter-paths",
            ["nixos-tests"],
            "Comma separated list of files or directories that form the filter",
        )
    ]

    @staticmethod
    def _normalize_path(path):
        return str(path).strip().strip("/")

    def _matches_filter(self, path):
        normalized_path = self._normalize_path(path)

        for filter_path in self.options["filter-paths"].value:
            normalized_filter = self._normalize_path(filter_path)
            if normalized_path == normalized_filter:
                return True
            if normalized_path.startswith(f"{normalized_filter}/"):
                return True

        return False

    def validate(self, commit):
        changed_files = getattr(commit, "changed_files", None)
        if changed_files is None:
            # Newer gitlint commit objects expose the touched paths directly via
            # `changed_files`. Older variants may only expose
            # `changed_files_stats`, a mapping keyed by changed path, so we fall
            # back to its keys when `changed_files` is unavailable.
            changed_files_stats = getattr(commit, "changed_files_stats", {})
            changed_files = changed_files_stats.keys()

        in_filter = []
        out_of_filter = []

        for path in changed_files:
            normalized_path = self._normalize_path(path)
            if self._matches_filter(normalized_path):
                in_filter.append(normalized_path)
            else:
                out_of_filter.append(normalized_path)

        if not in_filter or not out_of_filter:
            return

        filter_paths = ", ".join(self.options["filter-paths"].value)
        msg = (
                f"Commit mixes filtered paths ({filter_paths}) with paths outside the filter. "
                f"Inside filter: {in_filter}. Outside filter: {out_of_filter}"
        )
        return [RuleViolation(self.id, msg, line_nr=1)]
