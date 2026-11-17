# Vendor builds

The add-on can be built for NVIDIA (default) or AMD graphics cards from the same source.
Rule: **every build carries everything that runs on that vendor's cards.** Vendor-neutral
parts are in both.

| Feature | Runs on NVIDIA | Runs on AMD | NVIDIA build | AMD build |
|---|---|---|---|---|
| EDAA TAA, motion vectors, HUD protection, ship detection | yes | yes | yes | yes |
| Intel XeSS | yes | yes | yes | yes |
| NVIDIA DLSS / DLAA | yes | no | yes | no |
| NVIDIA DLSS-G (Streamline) and Reflex | RTX 40/50 | no | yes | no |

Building: `cmake -S . -B build-amd -DEDAA_VENDOR=amd` (default `nvidia`). The AMD build
defines `EDAA_VENDOR_AMD=1`, leaves out the DLSS and Streamline SDKs and maps a saved
DLSS method to the EDAA TAA.
