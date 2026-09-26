# Firmware release output

`scripts/export_firmware.py` validates the current `FINAL` worktree build and
writes the latest binary and metadata to `release/latest/`.

The binary and generated metadata are intentionally ignored by Git. The export
always reads `build/xiaozhi.bin` from the current worktree.
