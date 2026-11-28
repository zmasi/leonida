# Sourced by the POSIX launchers. Search for versioned Python names on PATH so a new Python minor
# release works even if the system's unversioned python3 still points to an older interpreter.
exec_leonida_python() {
    remaining=${PATH-}
    best_minor=11
    best_python=
    while :; do
        directory=${remaining%%:*}
        for candidate in "${directory:-.}"/python3.*; do
            [ -f "$candidate" ] && [ -x "$candidate" ] || continue
            name=${candidate##*/}
            minor=${name#python3.}
            case $minor in
                0*|''|*[!0-9]*) continue ;;
            esac
            # Invoke by name to keep PATH precedence if that version appears in more than one directory.
            if [ "$minor" -gt "$best_minor" ] 2>/dev/null && leonida_python_is_supported "$name"; then
                best_minor=$minor
                best_python=$name
            fi
        done
        case $remaining in
            *:*) remaining=${remaining#*:} ;;
            *) break ;;
        esac
    done

    if [ -n "$best_python" ]; then
        exec "$best_python" -I "$@"
    fi
    for name in python3 python; do
        if leonida_python_is_supported "$name"; then
            exec "$name" -I "$@"
        fi
    done
}

leonida_python_is_supported() {
    "$1" -I -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' >/dev/null 2>&1
}
