# Copies the files in FILES (separated by |) into DEST. file(COPY) keeps symbolic links as links,
# where cmake -E copy_if_different copied each name of a library as a full copy of its file.
string(REPLACE "|" ";" _files "${FILES}")
file(COPY ${_files} DESTINATION "${DEST}")
