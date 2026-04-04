# embed_skills.cmake — convert SKILLS.md to a C string literal
file(READ "${INPUT}" content)
# Escape backslashes, then quotes, then newlines
string(REPLACE "\\" "\\\\" content "${content}")
string(REPLACE "\"" "\\\"" content "${content}")
string(REPLACE "\n" "\\n\"\n\"" content "${content}")
file(WRITE "${OUTPUT}" "/* auto-generated from SKILLS.md — do not edit */\n")
file(APPEND "${OUTPUT}" "static const char skills_md[] =\n\"${content}\";\n")
