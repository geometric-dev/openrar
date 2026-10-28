# Extract lines 4-218 (1-indexed) from forward_arrival_full.cpp
# 0-indexed: lines 3-217 inclusive

with open('forward_arrival_full.cpp', 'r', encoding='utf-8') as f:
    new_lines = f.readlines()

# Extract evaluate_forward_arrival (lines 4-218 1-indexed -> 0-indexed 3-217)
new_func = new_lines[3:218]  # 0-indexed: 3 to 217 inclusive

with open('src/compress/compressor50.cpp', 'r', encoding='utf-8') as f:
    orig_lines = f.readlines()

# Replace lines 744-959 (1-indexed) -> 0-indexed: 743 to 958
new_lines = orig_lines[:743] + new_lines[3:218] + orig_lines[959:]

with open('src/compress/compressor50.cpp', 'w', encoding='utf-8') as f:
    f.writelines(new_lines)

print("Done!")