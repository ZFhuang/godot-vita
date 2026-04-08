#!/usr/bin/env bash

# Unified local static checks script for godot-vita.
# Mirrors the checks in .github/workflows/static_checks.yml
#
# By default, only checks files changed in the last 3 commits (incremental mode).
# Use -a for full repo scan, or -n N to customize the commit range.
#
# Usage:
#   ./scripts/static_checks.sh          # Incremental: check last 3 commits' files
#   ./scripts/static_checks.sh -a       # Full: check entire repo (slow)
#   ./scripts/static_checks.sh -n 5     # Incremental: check last 5 commits' files
#   ./scripts/static_checks.sh -c 1,3   # Run only check 1 and 3
#   ./scripts/static_checks.sh -l       # List available checks
#   ./scripts/static_checks.sh -h       # Show help

set -uo pipefail

# --- Color helpers ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# --- Ensure we run from the repo root ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT" || { echo "Failed to cd to repo root: $REPO_ROOT"; exit 1; }

# --- Mode settings ---
FULL_MODE=false
NUM_COMMITS=3

# --- Changed files cache (populated lazily) ---
CHANGED_FILES=""
CHANGED_FILES_LOADED=false

get_changed_files() {
    if [ "$CHANGED_FILES_LOADED" = true ]; then
        return
    fi
    # Get files changed in the last N commits + any uncommitted/staged changes
    local committed_files
    committed_files=$(git diff --name-only HEAD~"${NUM_COMMITS}" HEAD 2>/dev/null || echo "")
    local uncommitted_files
    uncommitted_files=$(git diff --name-only HEAD 2>/dev/null || echo "")
    local staged_files
    staged_files=$(git diff --name-only --cached 2>/dev/null || echo "")

    # Merge, deduplicate, and filter to only existing files
    CHANGED_FILES=$(printf '%s\n%s\n%s' "$committed_files" "$uncommitted_files" "$staged_files" \
        | sort -u | while IFS= read -r f; do
            [ -n "$f" ] && [ -f "$f" ] && echo "$f"
        done)
    CHANGED_FILES_LOADED=true
}

# Filter changed files by extensions (arguments are extensions like ".cpp" ".h")
filter_by_ext() {
    local result=""
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        for ext in "$@"; do
            if [[ "$f" == *"$ext" ]]; then
                echo "$f"
                break
            fi
        done
    done <<< "$CHANGED_FILES"
}

# Filter changed files by pattern (grep -E pattern)
filter_by_pattern() {
    local pattern="$1"
    echo "$CHANGED_FILES" | grep -E "$pattern" || true
}

# --- Check definitions ---
CHECKS=(
    "1:.gitignore validation:check_gitignore"
    "2:File format (UTF-8, line endings, trailing spaces):check_file_format"
    "3:Python style (black):check_black"
    "4:Class reference XML schema:check_xml_schema"
    "5:Documentation generation (dry-run):check_docs"
    "6:C/C++ style (clang-format) + copyright headers:check_clang_format"
    "7:Header guards:check_header_guards"
)

# =====================================================================
# Check functions - Full mode (delegates to original scripts)
# =====================================================================

check_gitignore_full() {
    bash ./misc/scripts/gitignore_check.sh
}

check_file_format_full() {
    bash ./misc/scripts/file_format.sh
}

check_black_full() {
    bash ./misc/scripts/black_format.sh
}

check_xml_schema_full() {
    xmllint --noout --schema doc/class.xsd doc/classes/*.xml modules/*/doc_classes/*.xml 2>&1
}

check_docs_full() {
    python3 doc/tools/make_rst.py --dry-run doc/classes modules
}

check_clang_format_full() {
    bash ./misc/scripts/clang_format.sh
}

check_header_guards_full() {
    bash ./misc/scripts/header_guards.sh
}

# =====================================================================
# Check functions - Incremental mode (only changed files)
# =====================================================================

check_gitignore_incremental() {
    get_changed_files

    if [ -z "$CHANGED_FILES" ]; then
        echo "No changed files to check."
        return 0
    fi

    echo -e ".gitignore validation (incremental)..."
    local has_error=false
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        result=$(git -c core.ignorecase=true check-ignore --verbose --no-index "$f" 2>/dev/null | \
            awk -F ':' '{ if ($3 !~ /^!/) print $0 }')
        if [ -n "$result" ]; then
            echo "$result"
            has_error=true
        fi
    done <<< "$CHANGED_FILES"

    if [ "$has_error" = true ]; then
        return 1
    fi
    return 0
}

check_file_format_incremental() {
    get_changed_files

    if [ -z "$CHANGED_FILES" ]; then
        echo "No changed files to check."
        return 0
    fi

    rm -f utf8-validation.txt patch.patch

    while IFS= read -r f; do
        [ -z "$f" ] && continue
        # Apply the same exclusions as file_format.sh
        [[ "$f" == *"csproj" ]] && continue
        [[ "$f" == *"sln" ]] && continue
        [[ "$f" == *".bat" ]] && continue
        [[ "$f" == *"patch" ]] && continue
        [[ "$f" == *"pot" ]] && continue
        [[ "$f" == *"po" ]] && continue
        [[ "$f" == "thirdparty/"* ]] && continue
        [[ "$f" == *"/thirdparty/"* ]] && continue
        [[ "$f" == "platform/android/java/lib/src/com/google"* ]] && continue
        [[ "$f" == *"-so_wrap."* ]] && continue

        # Check if it's a text file (skip binary)
        if ! git grep -qIl '' -- "$f" 2>/dev/null; then
            continue
        fi

        isutf8 "$f" >> utf8-validation.txt 2>&1
        dos2unix "$f" 2> /dev/null
        perl -i -ple 's/\s*$//g' "$f"
        perl -i -pe 's/\x20== true//g' "$f"
    done <<< "$CHANGED_FILES"

    git diff --color > patch.patch

    if [ ! -s utf8-validation.txt ] && [ ! -s patch.patch ]; then
        printf "Files in this commit comply with the formatting rules.\n"
        rm -f patch.patch utf8-validation.txt
        return 0
    fi

    if [ -s utf8-validation.txt ]; then
        printf "\n*** The following files contain invalid UTF-8 character sequences:\n\n"
        cat utf8-validation.txt
    fi
    if [ -s patch.patch ]; then
        printf "\n*** The following differences were found between the code "
        printf "and the formatting rules:\n\n"
        cat patch.patch
    fi
    rm -f utf8-validation.txt patch.patch
    printf "\n*** Aborting, please fix your commit(s) with 'git commit --amend' or 'git rebase -i <hash>'\n"
    return 1
}

check_black_incremental() {
    get_changed_files

    # Filter for Python files (*.py, SConstruct, SCsub)
    local py_files
    py_files=$(echo "$CHANGED_FILES" | while IFS= read -r f; do
        [ -z "$f" ] && continue
        [[ "$f" == "thirdparty/"* ]] && continue
        [[ "$f" == *"/thirdparty/"* ]] && continue
        case "$f" in
            *.py|*/SConstruct|*/SCsub|SConstruct|SCsub) echo "$f" ;;
        esac
    done)

    if [ -z "$py_files" ]; then
        echo "No changed Python files to check."
        return 0
    fi

    echo -e "Formatting Python files..."
    echo "$py_files" | xargs black -l 120

    git diff --color > patch.patch

    if [ ! -s patch.patch ]; then
        printf "Files in this commit comply with the black style rules.\n"
        rm -f patch.patch
        return 0
    fi

    printf "\n*** The following differences were found between the code "
    printf "and the formatting rules:\n\n"
    cat patch.patch
    printf "\n*** Aborting, please fix your commit(s) with 'git commit --amend' or 'git rebase -i <hash>'\n"
    rm -f patch.patch
    return 1
}

check_xml_schema_incremental() {
    get_changed_files

    # Filter for XML doc files
    local xml_files
    xml_files=$(echo "$CHANGED_FILES" | grep -E '(^doc/classes/.*\.xml$|/doc_classes/.*\.xml$)' || true)

    if [ -z "$xml_files" ]; then
        echo "No changed XML doc files to check."
        return 0
    fi

    echo "$xml_files" | xargs xmllint --noout --schema doc/class.xsd 2>&1
}

check_docs_incremental() {
    get_changed_files

    # If any XML doc files or the make_rst.py tool changed, run the check
    local doc_files
    doc_files=$(echo "$CHANGED_FILES" | grep -E '(^doc/|/doc_classes/|make_rst\.py)' || true)

    if [ -z "$doc_files" ]; then
        echo "No changed documentation files to check."
        return 0
    fi

    python3 doc/tools/make_rst.py --dry-run doc/classes modules
}

check_clang_format_incremental() {
    get_changed_files

    local CLANG_FORMAT_FILE_EXTS=(".c" ".h" ".cpp" ".hpp" ".cc" ".hh" ".cxx" ".m" ".mm" ".inc" ".java" ".glsl")

    local found_files=false

    while IFS= read -r f; do
        [ -z "$f" ] && continue
        # Apply the same exclusions as clang_format.sh
        [[ "$f" == "thirdparty"* ]] && continue
        [[ "$f" == "platform/android/java/lib/src/com/google"* ]] && continue
        [[ "$f" == *"-so_wrap."* ]] && continue

        for extension in "${CLANG_FORMAT_FILE_EXTS[@]}"; do
            if [[ "$f" == *"$extension" ]]; then
                found_files=true
                clang-format --Wno-error=unknown -i "$f"
                # Fix copyright headers, but not all files get them.
                if [[ "$f" == *"inc" ]]; then
                    break
                elif [[ "$f" == *"glsl" ]]; then
                    break
                elif [[ "$f" == *"theme_data.h" ]]; then
                    break
                elif [[ "$f" == "platform/android/java/lib/src/org/godotengine/godot/gl/GLSurfaceView"* ]]; then
                    break
                elif [[ "$f" == "platform/android/java/lib/src/org/godotengine/godot/gl/EGLLogWrapper"* ]]; then
                    break
                elif [[ "$f" == "platform/android/java/lib/src/org/godotengine/godot/utils/ProcessPhoenix"* ]]; then
                    break
                fi
                python3 misc/scripts/copyright_headers.py "$f"
                break
            fi
        done
    done <<< "$CHANGED_FILES"

    if [ "$found_files" = false ]; then
        echo "No changed C/C++ files to check."
        return 0
    fi

    git diff --color > patch.patch

    if [ ! -s patch.patch ]; then
        printf "Files in this commit comply with the clang-format style rules.\n"
        rm -f patch.patch
        return 0
    fi

    printf "\n*** The following differences were found between the code "
    printf "and the formatting rules:\n\n"
    cat patch.patch
    printf "\n*** Aborting, please fix your commit(s) with 'git commit --amend' or 'git rebase -i <hash>'\n"
    rm -f patch.patch
    return 1
}

check_header_guards_incremental() {
    get_changed_files

    # Filter for .h files only
    local h_files
    h_files=$(filter_by_ext ".h")

    if [ -z "$h_files" ]; then
        echo "No changed header files to check."
        return 0
    fi

    while IFS= read -r file; do
        [ -z "$file" ] && continue
        [[ "$file" == "thirdparty/"* || "$file" == *"/thirdparty/"* ]] && continue
        [[ "$file" == *".gen.h" || "$file" == *"-so_wrap.h" ]] && continue
        [[ "$file" == *"thread.h" || "$file" == *"platform_config.h" ]] && continue

        # Prepend ./ if not already present (header_guards.sh uses ./ prefix)
        local fpath="$file"
        [[ "$fpath" != ./* ]] && fpath="./$fpath"

        bname=$(basename "$fpath" .h)

        prefix=
        if [[ "$fpath" == "./modules/gdnative/"*"/register_types.h" ]]; then
            module=$(echo "$fpath" | sed "s@.*modules/gdnative/\([^/]*\).*@\1@")
            prefix="${module^^}_"
        elif [[ "$fpath" == "./modules/"*"/register_types.h" ]]; then
            module=$(echo "$fpath" | sed "s@.*modules/\([^/]*\).*@\1@")
            prefix="${module^^}_"
        fi
        if [[ "$fpath" == "./platform/"*"/api/api.h" || "$fpath" == "./platform/"*"/export/"* ]]; then
            platform=$(echo "$fpath" | sed "s@.*platform/\([^/]*\).*@\1@")
            prefix="${platform^^}_"
        fi
        if [[ "$fpath" == "./modules/mono/utils/"* && "$bname" != *"mono"* ]]; then prefix="MONO_"; fi
        if [[ "$fpath" == "./modules/gdnative/include/gdnative/"* ]]; then prefix="GDNATIVE_"; fi

        suffix=
        if [[ "$fpath" == *"ustring.h" ]]; then suffix="_GODOT"; fi

        guard="${prefix}${bname^^}${suffix}_H"

        sed -i "$fpath" -e "0,/ifndef/s/#ifndef.*/\n#ifndef $guard/"
        sed -i "$fpath" -e "0,/define/s/#define.*/#define $guard\n/"
        sed -i "$fpath" -e "$ s/#endif.*/\n#endif \/\/ $guard/"
        sed -i "$fpath" -e "/^$/N;/^\n$/D"
    done <<< "$h_files"

    diff=$(git diff --color)

    if [ -z "$diff" ]; then
        printf "Files in this commit comply with the header guards formatting rules.\n"
        return 0
    fi

    printf "\n*** The following differences were found between the code "
    printf "and the header guards formatting rules:\n\n"
    echo "$diff"
    printf "\n*** Aborting, please fix your commit(s) with 'git commit --amend' or 'git rebase -i <hash>'\n"
    return 1
}

# =====================================================================
# Dispatcher: choose full or incremental
# =====================================================================

check_gitignore()      { if [ "$FULL_MODE" = true ]; then check_gitignore_full;      else check_gitignore_incremental;      fi; }
check_file_format()    { if [ "$FULL_MODE" = true ]; then check_file_format_full;    else check_file_format_incremental;    fi; }
check_black()          { if [ "$FULL_MODE" = true ]; then check_black_full;          else check_black_incremental;          fi; }
check_xml_schema()     { if [ "$FULL_MODE" = true ]; then check_xml_schema_full;     else check_xml_schema_incremental;     fi; }
check_docs()           { if [ "$FULL_MODE" = true ]; then check_docs_full;           else check_docs_incremental;           fi; }
check_clang_format()   { if [ "$FULL_MODE" = true ]; then check_clang_format_full;   else check_clang_format_incremental;   fi; }
check_header_guards()  { if [ "$FULL_MODE" = true ]; then check_header_guards_full;  else check_header_guards_incremental;  fi; }

# --- Dependency check ---
check_dependencies() {
    local missing=()
    command -v dos2unix  >/dev/null 2>&1 || missing+=("dos2unix")
    command -v isutf8    >/dev/null 2>&1 || missing+=("isutf8 (moreutils)")
    command -v xmllint   >/dev/null 2>&1 || missing+=("xmllint (libxml2-utils)")
    command -v clang-format >/dev/null 2>&1 || missing+=("clang-format-16")
    command -v black     >/dev/null 2>&1 || missing+=("black")
    command -v python3   >/dev/null 2>&1 || missing+=("python3")

    if [ ${#missing[@]} -gt 0 ]; then
        echo -e "${RED}Missing dependencies:${NC}"
        for dep in "${missing[@]}"; do
            echo -e "  ${YELLOW}✗${NC} $dep"
        done
        echo ""
        echo -e "Install with:"
        echo -e "  ${CYAN}sudo apt-get install -y dos2unix moreutils libxml2-utils clang-format-16${NC}"
        echo -e "  ${CYAN}sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-16 100${NC}"
        echo -e "  ${CYAN}pip3 install black==24.10.0${NC}"
        echo ""
        return 1
    fi
    return 0
}

# --- Usage ---
usage() {
    echo -e "${BOLD}Godot-Vita Static Checks${NC}"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -a          Full mode: check entire repo (slow, same as CI)"
    echo "  -n N        Incremental: check files changed in last N commits (default: 3)"
    echo "  -l          List all available checks"
    echo "  -c CHECKS   Run only specified checks (comma-separated IDs, e.g. -c 1,3,6)"
    echo "  -s          Skip dependency check"
    echo "  -h          Show this help"
    echo ""
    echo "Examples:"
    echo "  $0              # Incremental: check last 3 commits' changed files"
    echo "  $0 -a           # Full repo scan (mirrors CI)"
    echo "  $0 -n 5         # Check last 5 commits' changed files"
    echo "  $0 -c 1,3       # Run only .gitignore and black checks"
    echo "  $0 -a -c 6      # Full clang-format check"
}

list_checks() {
    echo -e "${BOLD}Available checks:${NC}"
    echo ""
    for entry in "${CHECKS[@]}"; do
        IFS=':' read -r id name func <<< "$entry"
        echo -e "  ${CYAN}${id}${NC}  $name"
    done
    echo ""
    echo -e "Use ${CYAN}-c 1,3,6${NC} to run specific checks."
}

# --- Main ---
SELECTED_CHECKS=""
SKIP_DEPS=false

while getopts "lac:n:sh" opt; do
    case $opt in
        l) list_checks; exit 0 ;;
        a) FULL_MODE=true ;;
        c) SELECTED_CHECKS="$OPTARG" ;;
        n) NUM_COMMITS="$OPTARG" ;;
        s) SKIP_DEPS=true ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║     Godot-Vita Local Static Checks       ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
echo ""

if [ "$FULL_MODE" = true ]; then
    echo -e "  Mode: ${YELLOW}FULL${NC} (entire repo)"
else
    echo -e "  Mode: ${GREEN}INCREMENTAL${NC} (last ${CYAN}${NUM_COMMITS}${NC} commits + uncommitted changes)"
fi
echo ""

# Check dependencies
if [ "$SKIP_DEPS" = false ]; then
    echo -e "${CYAN}Checking dependencies...${NC}"
    if ! check_dependencies; then
        echo -e "${RED}Aborting due to missing dependencies. Use -s to skip this check.${NC}"
        exit 1
    fi
    echo -e "${GREEN}All dependencies found.${NC}"
    echo ""
fi

# In incremental mode, show which files will be checked
if [ "$FULL_MODE" = false ]; then
    get_changed_files
    file_count=$(echo "$CHANGED_FILES" | grep -c '.' || true)
    if [ "$file_count" -eq 0 ]; then
        echo -e "${GREEN}No files changed in the last ${NUM_COMMITS} commits. Nothing to check.${NC}"
        exit 0
    fi
    echo -e "${CYAN}Changed files (${file_count}):${NC}"
    echo "$CHANGED_FILES" | head -20 | while IFS= read -r f; do
        echo -e "  $f"
    done
    if [ "$file_count" -gt 20 ]; then
        echo -e "  ${YELLOW}... and $((file_count - 20)) more${NC}"
    fi
    echo ""
fi

# Determine which checks to run
declare -a run_ids
if [ -n "$SELECTED_CHECKS" ]; then
    IFS=',' read -ra run_ids <<< "$SELECTED_CHECKS"
else
    for entry in "${CHECKS[@]}"; do
        IFS=':' read -r id _ _ <<< "$entry"
        run_ids+=("$id")
    done
fi

# Stage current changes so that formatting checks only detect NEW changes
# produced by the check scripts themselves (via unstaged git diff).
AUTO_STAGED=false
if ! git diff --quiet 2>/dev/null; then
    echo -e "${CYAN}Staging current changes so checks can detect formatting violations...${NC}"
    git add -u
    AUTO_STAGED=true
    echo ""
fi

# Run checks
total=0
passed=0
failed=0
failed_names=()

for run_id in "${run_ids[@]}"; do
    found=false
    for entry in "${CHECKS[@]}"; do
        IFS=':' read -r id name func <<< "$entry"
        if [ "$id" = "$run_id" ]; then
            found=true
            total=$((total + 1))
            echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
            echo -e "${CYAN}[$id/${#CHECKS[@]}]${NC} ${BOLD}$name${NC}"
            echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

            if $func; then
                echo -e "${GREEN}✔ PASSED${NC}: $name"
                passed=$((passed + 1))
            else
                echo -e "${RED}✘ FAILED${NC}: $name"
                failed=$((failed + 1))
                failed_names+=("$name")
            fi
            echo ""
            break
        fi
    done
    if [ "$found" = false ]; then
        echo -e "${YELLOW}Warning: Unknown check ID '$run_id', skipping.${NC}"
    fi
done

# Restore working tree state: unstage auto-staged files and undo any
# formatting changes made by the check scripts.
if [ "$AUTO_STAGED" = true ]; then
    # Reset index to unstage, keeping working tree changes
    git reset HEAD --quiet 2>/dev/null || true
fi

# Summary
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "${BOLD}                 SUMMARY                  ${NC}"
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "  Total:  $total"
echo -e "  ${GREEN}Passed: $passed${NC}"
echo -e "  ${RED}Failed: $failed${NC}"

if [ $failed -gt 0 ]; then
    echo ""
    echo -e "${RED}Failed checks:${NC}"
    for name in "${failed_names[@]}"; do
        echo -e "  ${RED}✘${NC} $name"
    done
    echo ""
    echo -e "${YELLOW}Tip: Fix issues and re-run, or use 'git checkout .' to discard formatting changes.${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}All checks passed! ✔${NC}"
    exit 0
fi
