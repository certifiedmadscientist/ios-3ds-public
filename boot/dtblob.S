/* Apple's flattened n45ap DeviceTree (tools/extract_devicetree.py). */
    .section .rodata
    .align 2
    .global n45ap_dt_start
    .global n45ap_dt_end
n45ap_dt_start:
    .incbin "n45ap.devicetree"
n45ap_dt_end:
