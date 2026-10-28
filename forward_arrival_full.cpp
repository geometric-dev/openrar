// Modified evaluate_forward_arrival function with proper DP
// This is a standalone replacement for the function in compressor50.cpp

Compressor50::ForwardArrivalDecision Compressor50::evaluate_forward_arrival(
    core::uint64 cur, size_t match_len, core::int64 match_candidate,
    size_t rep_best_len, int rep_idx, size_t cur_max_lz) {
    // Local arrival buffer to avoid member variable issues
    struct LocalArrivalNode {
        core::uint32 cost{0xFFFFFFFFu};
        core::uint8 from_offset{0};
        core::uint8 token_type{0};
        core::uint16 token_len{0};
        core::uint32 token_dist{0};
        size_t old_dist[4];
        size_t last_len{0};
    };
    constexpr int H = 16;
    LocalArrivalNode arrival[H + 1];

    // Reset arrival buffer
    for (int i = 0; i <= H; ++i) {
        arrival[i].cost = DP_INF;
        arrival[i].from_offset = 0;
        arrival[i].token_type = 0;
        arrival[i].token_len = 0;
        arrival[i].token_dist = 0;
        for (int d = 0; d < 4; ++d) arrival[i].old_dist[d] = old_dist_[d];
        arrival[i].last_len = last_length_;
    }
    arrival[0].cost = 0;

    const core::byte* buf_cur = &buf_data_[static_cast<size_t>(cur - pos_base_)];
    size_t avail = avail_at(cur);
    if (avail > cur_max_lz) avail = cur_max_lz;

    // Offset 0: evaluate all candidates
    // Helper to store candidate at clamped horizon index while preserving actual token length
    auto store_candidate = [&](int token_type, size_t token_len, core::uint32 token_dist,
                               core::uint32 cost, const size_t* old_dist_src, size_t last_len_src) {
        int idx = token_len > H ? H : static_cast<int>(token_len);
        if (cost < arrival[idx].cost) {
            arrival[idx].cost = cost;
            arrival[idx].from_offset = 0;
            arrival[idx].token_type = static_cast<core::uint8>(token_type);
            arrival[idx].token_len = static_cast<core::uint16>(token_len);
            arrival[idx].token_dist = token_dist;
            for (int d = 0; d < 4; ++d) arrival[idx].old_dist[d] = old_dist_src[d];
            arrival[idx].last_len = last_len_src;
        }
    };

    // 1. Literal
    core::uint32 lit_cost = price_literal(buf_cur[0]);
    store_candidate(0, 1, 0, lit_cost, old_dist_, last_length_);

    // 2. Rep0..3 (token types 2..5 match Compressor50Token::TokenType)
    if (rep_best_len >= 2 && rep_idx >= 0) {
        // Wire-legal floor for rep: len >= 2 (already checked by rep_best_len >= 2)
        core::uint32 cost = price_rep(static_cast<core::uint32>(rep_idx), rep_best_len);
        store_candidate(2 + rep_idx, rep_best_len, static_cast<core::uint32>(rep_idx),
                        cost, old_dist_, last_length_);
    }

    // 3. Slot 257 / LastLen (token type 6 matches Compressor50Token::TokenType::LastLen)
    if (last_length_ != 0 && rep_best_len == static_cast<size_t>(last_length_) && rep_idx == 0) {
        core::uint32 cost = price_slot257();
        store_candidate(6, rep_best_len, 0, cost, old_dist_, last_length_);
    }

    // 4. Fresh match (use pre-computed match)
    if (match_len >= 3 && match_candidate >= 0) {
        size_t mlen = match_len;
        size_t mdist = static_cast<size_t>(cur - static_cast<core::uint64>(match_candidate));
        if (mlen > avail) mlen = avail;
        // Wire-legal floor: fresh match requires len >= inc + 2
        core::uint32 inc = length_increment(mdist);
        if (mlen >= static_cast<size_t>(inc) + 2) {
            core::uint32 cost = price_match(mlen, mdist);
            store_candidate(1, mlen, static_cast<core::uint32>(mdist), cost, old_dist_, last_length_);
        }
    }

    // ── Forward DP over horizon H ────────────────────────────────────────────────
    // Propagate best costs through offsets 0..H-1 using buffer lookahead.
    for (int i = 0; i < H; ++i) {
        if (arrival[i].cost == DP_INF) continue;  // unreachable offset

        core::uint64 pos = cur + i;
        size_t avail_at_pos = avail_at(pos);
        if (avail_at_pos == 0) continue;
        if (avail_at_pos > cur_max_lz) avail_at_pos = cur_max_lz;

        const core::byte* buf_pos = &buf_data_[static_cast<size_t>(pos - pos_base_)];
        size_t avail = avail_at_pos;

        // Evaluate all token candidates starting at pos
        // For i == 0, pre-computed match/rep data already stored in arrival[1..H]
        // For i > 0, use buffer lookahead (no fresh match finding)

        // 1. Literal
        if (avail >= 1) {
            core::uint32 lit_cost = price_literal(buf_pos[0]);
            core::uint32 new_cost = arrival[i].cost + lit_cost;
            int j = i + 1 > 16 ? 16 : i + 1;
            if (new_cost < arrival[j].cost) {
                arrival[j].cost = new_cost;
                arrival[j].from_offset = i;
                arrival[j].token_type = 0;
                arrival[j].token_len = 1;
                arrival[j].token_dist = 0;
                for (int d = 0; d < 4; ++d) arrival[j].old_dist[d] = arrival[i].old_dist[d];
                arrival[j].last_len = arrival[i].last_len;
            }
        }

        // 2. Rep0..3
        for (int r = 0; r < 4; ++r) {
            size_t d = arrival[i].old_dist[r];
            if (d != static_cast<size_t>(-1) && d != 0 && d <= pos - pos_base_ && d <= max_dist_) {
                if (avail >= 2 && buf_pos[0] == buf_pos[-static_cast<core::int64>(d)]) {
                    size_t rep_len = rep_length(pos, d, avail);
                    if (rep_len >= 2 && rep_len <= cur_max_lz) {
                        core::uint32 cost = price_rep(r, rep_len);
                        core::uint32 new_cost = arrival[i].cost + cost;
                        int j = i + static_cast<int>(rep_len) > 16 ? 16 : i + static_cast<int>(rep_len);
                        if (new_cost < arrival[j].cost) {
                            arrival[j].cost = new_cost;
                            arrival[j].from_offset = i;
                            arrival[j].token_type = static_cast<core::uint8>(2 + r);
                            arrival[j].token_len = static_cast<core::uint16>(rep_len);
                            arrival[j].token_dist = static_cast<core::uint32>(r);
                            for (int d = 0; d < 4; ++d) arrival[j].old_dist[d] = arrival[i].old_dist[d];
                            arrival[j].last_len = arrival[i].last_len;
                        }
                    }
                }
            }

        // 3. Slot 257 / LastLen
        if (arrival[i].last_len != 0) {
            size_t d = arrival[i].old_dist[0];
            if (d != static_cast<size_t>(-1) && d != 0 && d <= pos - pos_base_ && d <= max_dist_) {
                if (avail >= arrival[i].last_len && buf_pos[0] == buf_pos[-static_cast<core::int64>(d)]) {
                    size_t rep0_len = rep_length(pos, d, avail);
                    if (rep0_len == arrival[i].last_len && rep0_len <= cur_max_lz) {
                        core::uint32 cost = price_slot257();
                        core::uint32 new_cost = arrival[i].cost + cost;
                        int j = i + static_cast<int>(rep0_len) > 16 ? 16 : i + static_cast<int>(rep0_len);
                        if (new_cost < arrival[j].cost) {
                            arrival[j].cost = new_cost;
                            arrival[j].from_offset = i;
                            arrival[j].token_type = 6;
                            arrival[j].token_len = static_cast<core::uint16>(rep0_len);
                            arrival[j].token_dist = 0;
                            for (int d = 0; d < 4; ++d) arrival[j].old_dist[d] = arrival[i].old_dist[d];
                            arrival[j].last_len = arrival[i].last_len;
                        }
                    }
                }
            }

        // 4. Fresh match - only at offset 0 where we have pre-computed data
        // (Already stored in arrival[] during initial candidate evaluation)
    }
    // ────────────────────────────────────────────────────────────────────────

    // Select best arrival: minimum total cost with tie-breaker for longer tokens
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
}
}
}

// Commits the selected arrival token decision and updates encoder state
// Token types match Compressor50Token::TokenType: Literal=0, Match=1, Rep0=2, Rep1=3, Rep2=4, Rep3=5, LastLen=6
void Compressor50::commit_arrival_token(const ForwardArrivalDecision& decision) {
    switch (decision.token_type) {
    case 0:  // Literal
        add_literal(buf_data_[static_cast<size_t>(cur_ - pos_base_)]);
        insert_position(cur_);
        cur_++;
        input_since_block_++;
        unhashed_pos_ = cur_;
        break;
    case 1:  // Match
        {
            // add_match returns the actual encoded length (may be shorter due to truncation/admissibility)
            size_t actual_len = add_match(decision.token_len, decision.token_dist);
            if (actual_len == 0) {
                // Match was filtered out (inadmissible), fall back to literal
                add_literal(buf_data_[static_cast<size_t>(cur_ - pos_base_)]);
                insert_position(cur_);
                cur_++;
                input_since_block_++;
                unhashed_pos_ = cur_;
            } else {
                core::uint64 end = cur_ + actual_len;
                if (end > src_loaded_) end = src_loaded_;
                hash_range(cur_, end);
                cur_ = end;
                input_since_block_ += actual_len;
                unhashed_pos_ = cur_;
            }
        }
        break;
    case 2:  // Rep0
    case 3:  // Rep1
    case 4:  // Rep2
    case 5:  // Rep3
        add_rep(decision.token_type - 2, decision.token_len);
        {
            core::uint64 end = cur_ + decision.token_len;
            if (end > src_loaded_) end = src_loaded_;
            hash_range(cur_, end);
            cur_ = end;
            input_since_block_ += decision.token_len;
            unhashed_pos_ = cur_;
        }
        break;
    case 6:  // LastLen (slot 257)
        add_last_len();
        {
            core::uint64 end = cur_ + decision.token_len;
            if (end > src_loaded_) end = src_loaded_;
            hash_range(cur_, end);
            cur_ = end;
            input_since_block_ += decision.token_len;
            unhashed_pos_ = cur_;
        }
        break;
    }
}