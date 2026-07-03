# gen-static-properties.awk — turn a jwc_properties.ini into the C tables the
# phoneME share/properties config-db expects when built without USE_PROPERTIES_FROM_FS.
#
# The phoneME javacall port normally generates javacall_static_properties.c from
# properties.xml via Configurator.jar. That tool is not in our tree, so we derive
# the same three arrays directly from the merged jwc_properties.ini the MIDP build
# already produced. configdb keys entries as "section:key", matching the
# "internal:"/"application:" prefixes javacall_get_property joins on.
#
# Usage: awk -f gen-static-properties.awk jwc_properties.ini > javacall_static_properties.c

BEGIN { ns = 0 }

{ sub(/\r$/, "") }                 # tolerate CRLF
/^[ \t]*#/  { next }               # comment
/^[ \t]*$/  { next }               # blank

/^[ \t]*\[/ {                      # [section]
    line = $0
    sub(/^[ \t]*\[/, "", line)
    sub(/\].*$/, "", line)
    sec = line
    sections[ns] = sec
    ns++
    nk[sec] = 0
    next
}

{                                  # key = value
    line = $0
    eq = index(line, "=")
    if (eq == 0) next
    key = substr(line, 1, eq - 1)
    val = substr(line, eq + 1)
    gsub(/^[ \t]+|[ \t]+$/, "", key)
    gsub(/^[ \t]+|[ \t]+$/, "", val)
    if (key == "") next
    gsub(/\\/, "\\\\", key); gsub(/"/, "\\\"", key)
    gsub(/\\/, "\\\\", val); gsub(/"/, "\\\"", val)
    i = nk[sec]
    keys[sec, i] = key
    vals[sec, i] = val
    nk[sec] = i + 1
    next
}

END {
    print "/* Auto-generated from jwc_properties.ini. Do not edit. */"
    print ""
    for (s = 0; s < ns; s++) {
        sec = sections[s]
        printf "static char* keys_%d[] = {", s
        for (i = 0; i < nk[sec]; i++) printf " \"%s\",", keys[sec, i]
        print " 0 };"
        printf "static char* vals_%d[] = {", s
        for (i = 0; i < nk[sec]; i++) printf " \"%s\",", vals[sec, i]
        print " 0 };"
    }
    print ""
    printf "char* javacall_static_properties_sections[] = {"
    for (s = 0; s < ns; s++) printf " \"%s\",", sections[s]
    print " 0 };"
    printf "char** javacall_static_properties_keys[] = {"
    for (s = 0; s < ns; s++) printf " keys_%d,", s
    print " 0 };"
    printf "char** javacall_static_properties_values[] = {"
    for (s = 0; s < ns; s++) printf " vals_%d,", s
    print " 0 };"
}
