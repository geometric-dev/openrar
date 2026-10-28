import re

with open('src/compress/compressor50.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# Find the section where best_offset is selected and replace it
old_selection = """    // Select best arrival: minimum total cost with tie-breaker for longer tokens
    // This is more accurate than cost-per-byte since PriceModel costs already account for
    // token overhead. Prefer longer tokens when costs are close (advances further).
    core::uint32 best_cost = DP_INF;
    int best_offset = -1;
    for (int t = 1; t <= H; ++t) {
        if (arrival[t].cost < DP_INF) {
            size_t token_len = arrival[t].token_len > 0 ? arrival[t].token_len : t;
            if (arrival[t].cost < best_cost ||
                (arrival[t].cost == best_cost && token_len > 
                 (best_offset > 0 ? (arrival[best_offset].token_len > 0 ? arrival[best_offset].token_len : best_offset) : 0))) {
                best_cost = arrival[t].cost;
                best_offset = t;
            }
        }
    }

    // Fallback: if no multi-byte token found, use literal
    if (best_offset == -1) {
        best_offset = 1;
        best_cost = arrival[1].cost;
    }

    // Nice-length cutoff: return decision immediately if competitive
    // Use actual token_len for comparison
    size_t best_token_len = arrival[best_offset].token_len > 0 ? arrival[best_offset].token_len : best_offset;
    if (best_token_len >= nice_len_ ||
        (best_token_len >= 32 && arrival[best_offset].token_dist <= 256)) {
        return {arrival[best_offset].token_type,
                arrival[best_offset].token_len,
                arrival[best_offset].token_dist,
                best_cost};
    }

    // Selective offset 1 expansion (only if offset 0 was contested)
    // Check if there were multiple viable candidates at offset 0
    int viable_count = 0;
    for (int t = 1; t <= H; ++t) {
        if (arrival[t].cost < DP_INF) viable_count++;
    }
    if (viable_count > 1) {
        // Evaluate offset 1 candidates (simplified: just check literal)
        // In a full implementation, this would recurse, but for now we
        // just use the offset 0 decision which is already cost-aware.
    }

    return {arrival[best_offset].token_type,
            arrival[best_offset].token_len,
            arrival[best_offset].token_dist,
            best_cost};
}"""

new_selection = """    // ── Select optimal path: minimum total cost to any reachable offset ──
    // We find the minimum cost to reach ANY reachable offset, then trace back
    // to find the first token of that optimal path.
    core::uint32 best_cost = DP_INF;
    int best_target = -1;
    for (int t = 1; t <= H; ++t) {
        if (arrival[t].cost < best_cost) {
            best_cost = arrival[t].cost;
            best_target = t;
        }
    }
    if (best_target == -1) {
        // Fallback: literal (should not happen)
        return {0, 1, 0, lit_cost};
    }

    // Nice-length cutoff: if the optimal path's first token is competitive, return immediately
    // We need to trace back first to find the first token
    int cur_off = best_target;
    int first_token_type = 0;
    core::uint16 first_token_len = 0;
    core::uint32 first_token_dist = 0;
    while (cur_off > 0) {
        int prev = arrival[cur_off].from_offset;
        if (prev == 0) {
            first_token_type = arrival[cur_off].token_type;
            first_token_len = arrival[cur_off].token_len;
            first_token_dist = arrival[cur_off].token_dist;
            break;
        }
        cur_off = prev;
    }
    if (first_token_len == 0) {
        // Fallback: literal
        return {0, 1, 0, lit_cost};
    }

    // Nice-length cutoff: if the first token is a long match, commit
    size_t first_token_len_val = first_token_len;
    if (first_token_len_val >= nice_len_ ||
        (first_token_len_val >= 32 && first_token_dist <= 256)) {
        return {static_cast<core::uint32>(first_token_type),
                first_token_len,
                first_token_dist,
                best_cost};
    }

    // For now, return the first token of the optimal path
    // Future: implement recursive offset 1 expansion for true lookahead
    return {static_cast<core::uint32>(first_token_type),
            first_token_len,
            first_token_dist,
            best_cost};
}"""

# Do the replacement
if old_selection in content:
    content = content.replace(old_selection, new_selection)
    with open('src/compress/compressor50.cpp', 'w', encoding='utf-8') as f:
        f.write(content)
    print("Successfully replaced selection logic")
else:
    print("ERROR: Could not find the selection code to replace")
    # Debug: find the best_cost line
    idx = content.find("core::uint32 best_cost = DP_INF;")
    if idx >= 0:
        print(f"Found best_cost at {idx}")
        print(repr(content[idx:idx+500]))
    else:
        print("Could not find best_cost")