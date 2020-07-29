# SOtask5

Changing minix filesystem to work in 3 modes:
- mode A: file deletion is prohibited (if one tries to remove a file, they'll get EPERM)
- mode B: file is deleted after performing rm operation twice (first rm returns EINPROGRESS)
- mode C: create backup (.bak) file upon first delete operation, removes the file upon the second delete operation

The directory is in a mode iff it contains a mode file - file named '<mode, e.g. A>.mode'.
Deleting mode files always work normally (as if there was no mode at all).
If no mode file exists in the directory, removal operations are performed normally.
