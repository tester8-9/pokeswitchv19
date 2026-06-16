#pragma once

// SWSH AI Bridge V17 - PokeChange-inspired role/matchup switch policy.
//
// What V3 proved from the user's log:
// - The hook at 0x7D4BDC is reached during Ian/062 trainer battle.
// - We can write the AMX output fields (score and PokeChangeEnable).
// - Forcing those fields does NOT cause switching.
//
// New conclusion:
// The AMX readback output is not the final trainer action.  It only scores
// the current candidate in a higher-level candidate-selection state machine.
// V4 hooks that state machine directly, logging candidate IDs/scores and
// optionally forcing the final selected candidate ID for controlled tests.

#include "lib.hpp"
#include "external.hpp"
#include "symbols.hpp"
#include "config.hpp"
#include "loggers.hpp"

namespace AIBridge {
    constexpr bool kOffsetsMappedForSword132 = true;

    constexpr u64 kOutputReadbackPostOffset = 0x007D4BDC;
    constexpr u64 kCandidateListPostOffset  = 0x008E8E0C;
    constexpr u64 kCandidateScorePostOffset = 0x008E8E70;
    constexpr u64 kFinalSelectionPostOffset = 0x008E8F60;

    inline bool installed = false;
    inline u32 output_hits = 0;
    inline u32 list_hits = 0;
    inline u32 score_hits = 0;
    inline u32 final_hits = 0;

    // V6 practical switching-policy state.  This is intentionally simple and
    // configurable.  It does not try to pretend we fully understand every
    // battle structure yet; it just prevents the v5 proof mode from becoming
    // "switch every time forever".
    inline u64 policy_state_ptr = 0;
    inline u32 policy_forced_total = 0;
    inline u32 policy_forced_action2 = 0;
    inline u32 policy_forced_action3 = 0;
    inline u32 policy_forced_other = 0;
    inline u32 policy_cooldown = 0;
    inline u32 policy_last_preferred_action = 3;

    // V12 deferred switching: candidate_score fires before the full move-score
    // window is known.  Store the live candidate table, then allow the
    // readback/move-scoring hook to apply the switch policy after enough move
    // evaluations have been accumulated.
    inline u64 deferred_candidate_state_ptr = 0;
    inline u32 deferred_candidate_score_hit = 0;
    inline bool deferred_candidate_ready = false;
    inline bool deferred_policy_applied_this_window = false;

    static inline bool ShouldLog(u32 hit) {
        return global_config.ai_bridge.log_hits && hit <= global_config.ai_bridge.max_log_hits;
    }

    static inline u32 ClampCandidateCount(u32 count) {
        if (count > 16) return 16;
        return count;
    }

    static inline void DumpCandidateTable(const char* tag, u64 state_ptr, u32 hit) {
        if (!ShouldLog(hit)) return;
        if (state_ptr < 0x100000) {
            Logging.Log("[ai_bridge] %s hit=%u invalid_state=%016lx\n", tag, hit, state_ptr);
            return;
        }

        volatile u8* b = reinterpret_cast<volatile u8*>(state_ptr);
        volatile u32* w = reinterpret_cast<volatile u32*>(state_ptr);

        const u32 count = ClampCandidateCount(static_cast<u32>(b[0xC5]));
        const u32 cursor = static_cast<u32>(b[0xC6]);
        const u32 final_valid = static_cast<u32>(b[0xF8]);
        const u32 final_score = *reinterpret_cast<volatile u32*>(state_ptr + 0xFC);
        const u32 final_action = *reinterpret_cast<volatile u32*>(state_ptr + 0x100);

        Logging.Log("[ai_bridge] %s hit=%u state=%016lx count=%u cursor=%u final_valid=%u final_score=%u final_action=%u c0=%u c4=%u\n",
            tag, hit, state_ptr, count, cursor, final_valid, final_score, final_action,
            *reinterpret_cast<volatile u32*>(state_ptr + 0xC0), static_cast<u32>(b[0xC4]));

        u32 dump_count = global_config.ai_bridge.candidate_dump_count;
        if (dump_count > count) dump_count = count;
        if (dump_count > 16) dump_count = 16;

        for (u32 i = 0; i < dump_count; i++) {
            const u64 entry = state_ptr + 0xC8 + (static_cast<u64>(i) * 8);
            const u32 action_id = static_cast<u32>(*reinterpret_cast<volatile u8*>(entry + 0x0));
            const u32 enabled = static_cast<u32>(*reinterpret_cast<volatile u8*>(entry + 0x1));
            const u32 byte2 = static_cast<u32>(*reinterpret_cast<volatile u8*>(entry + 0x2));
            const u32 byte3 = static_cast<u32>(*reinterpret_cast<volatile u8*>(entry + 0x3));
            const u32 score = *reinterpret_cast<volatile u32*>(entry + 0x4);
            Logging.Log("[ai_bridge] %s cand[%u] action=%u enabled=%u b2=%u b3=%u score=%u raw=%02x %02x %02x %02x %08x\n",
                tag, i, action_id, enabled, byte2, byte3, score,
                action_id, enabled, byte2, byte3, score);
        }
        (void)w;
    }


    static inline bool LooksLikePtr(u64 ptr) {
        return ptr >= 0x100000;
    }

    static inline u16 ReadU16(u64 ptr, u32 offset) {
        if (!LooksLikePtr(ptr)) return 0;
        return *reinterpret_cast<volatile u16*>(ptr + offset);
    }

    static inline const char* KnownSpeciesName(u16 species) {
        switch (species) {
            case 620: return "Mienshao";
            case 475: return "Gallade";
            case 534: return "Conkeldurr";
            case 448: return "Lucario";
            case 675: return "Pangoro";
            case 815: return "Cinderace";
            case 861: return "Grimmsnarl";
            default: return "";
        }
    }

    static inline bool IsKnownProbeSpecies(u16 species) {
        return species == 620 || species == 475 || species == 534 || species == 448 ||
               species == 675 || species == 815 || species == 861;
    }

    static inline u16 SpeciesFromEvalTarget(u64 eval_target) {
        // V13 latest log showed the target Pokémon species as a u16 at +0x70:
        //   target ptr ...6270 +0x70 = 0x032F = 815 Cinderace
        //   target ptr ...6A10 +0x70 = 0x035D = 861 Grimmsnarl
        //   target ptr ...C590 +0x70 = 0x01DB = 475 Gallade
        return ReadU16(eval_target, 0x70);
    }

    static inline void ScanKnownSpecies(const char* tag, u64 ptr, u32 hit, u32 bytes_to_scan) {
        if (!global_config.ai_bridge.score_survey_mode) return;
        if (!ShouldLog(hit)) return;
        if (!LooksLikePtr(ptr)) return;
        if (bytes_to_scan > 0x200) bytes_to_scan = 0x200;
        for (u32 off = 0; off + 1 < bytes_to_scan; off += 2) {
            const u16 species = ReadU16(ptr, off);
            if (IsKnownProbeSpecies(species)) {
                Logging.Log("[ai_bridge] species_probe %s hit=%u ptr=%016lx off=0x%x species=%u/%s\n",
                    tag, hit, ptr, off, static_cast<u32>(species), KnownSpeciesName(species));
            }
        }
    }

    static inline u64 LoHiPtr(u64 base, u32 offset) {
        if (!LooksLikePtr(base)) return 0;
        const u32 lo = *reinterpret_cast<volatile u32*>(base + offset + 0x0);
        const u32 hi = *reinterpret_cast<volatile u32*>(base + offset + 0x4);
        return (static_cast<u64>(hi) << 32) | static_cast<u64>(lo);
    }

    static inline void DumpCandidatePointerRefs(u64 state_ptr, u32 hit) {
        if (!global_config.ai_bridge.score_survey_mode) return;
        if (!ShouldLog(hit)) return;
        if (!LooksLikePtr(state_ptr)) return;
        const u32 offsets[] = {0x68, 0x70, 0x78, 0x80, 0xA8, 0xB0, 0x128, 0x130};
        const char* names[] = {"cs_68", "cs_70", "cs_78", "cs_80", "cs_A8", "cs_B0", "cs_128", "cs_130"};
        for (u32 i = 0; i < 8; i++) {
            const u64 ptr = LoHiPtr(state_ptr, offsets[i]);
            if (LooksLikePtr(ptr)) {
                Logging.Log("[ai_bridge] candidate_ptr %s hit=%u off=0x%x ptr=%016lx\n", names[i], hit, offsets[i], ptr);
                ScanKnownSpecies(names[i], ptr, hit, 0x120);
            }
        }
    }

    static inline u32 ClampSurveyWords(u32 words) {
        if (words == 0) return 0;
        if (words > 96) return 96;
        return words;
    }

    static inline void DumpRawWords(const char* tag, u64 ptr, u32 hit, u32 words) {
        if (!global_config.ai_bridge.score_survey_mode) return;
        if (!ShouldLog(hit)) return;
        if (!LooksLikePtr(ptr)) {
            Logging.Log("[ai_bridge] %s hit=%u ptr=%016lx skipped\n", tag, hit, ptr);
            return;
        }
        const u32 count = ClampSurveyWords(words);
        if (count == 0) return;
        volatile u32* w = reinterpret_cast<volatile u32*>(ptr);
        for (u32 i = 0; i < count; i += 8) {
            Logging.Log("[ai_bridge] %s hit=%u ptr=%016lx +%03x=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                tag, hit, ptr, i * 4,
                w[i + 0], (i + 1 < count ? w[i + 1] : 0), (i + 2 < count ? w[i + 2] : 0), (i + 3 < count ? w[i + 3] : 0),
                (i + 4 < count ? w[i + 4] : 0), (i + 5 < count ? w[i + 5] : 0), (i + 6 < count ? w[i + 6] : 0), (i + 7 < count ? w[i + 7] : 0));
        }
    }


    struct MoveScoreRecord {
        u32 move_id;
        u32 target_low;
        u16 target_species;
        u16 pad;
        s32 score_sum;
        s32 score_min;
        s32 score_max;
        u32 count;
    };

    inline MoveScoreRecord move_records[32] = {};
    inline u32 move_record_count = 0;
    inline bool move_records_consumed = false;
    inline u32 move_summary_generation = 0;

    static inline void ClearMoveScoreRecords() {
        for (u32 i = 0; i < 32; i++) {
            move_records[i] = {};
        }
        move_record_count = 0;
        move_records_consumed = false;
        move_summary_generation++;
    }

    static inline s32 SignedScore(u32 score) {
        return static_cast<s32>(score);
    }

    static inline u64 HiLoPtrFromOut(u64 out, u32 offset) {
        // The readback output structure logs address-like values as high-word,
        // low-word pairs in the u32 dump, e.g. 00000021 8ceb6270 -> 0x218ceb6270.
        const u32 hi = *reinterpret_cast<volatile u32*>(out + offset + 0x0);
        const u32 lo = *reinterpret_cast<volatile u32*>(out + offset + 0x4);
        return (static_cast<u64>(hi) << 32) | static_cast<u64>(lo);
    }

    static inline u32 MoveIdFromOut(u64 out) {
        // V8 logs showed the current evaluated move id at readback_out_raw +0xB0.
        // Examples observed in the user's log:
        //   0x00FC Fake Out
        //   0x0172 Close Combat
        //   0x01D5 Wide Guard
        //   0x00C5 Detect
        return *reinterpret_cast<volatile u32*>(out + 0xB0);
    }

    static inline u32 TargetKeyFromOut(u64 out) {
        // Use the low 32 bits of the apparent target/eval-object pointer as a
        // stable per-target key.  This is enough for comparing target variants
        // in one battle; we do not need it to be globally stable.
        return *reinterpret_cast<volatile u32*>(out + 0xA4);
    }

    static inline void RecordMoveScore(u32 move_id, u32 target_key, u16 target_species, s32 score_delta) {
        if (move_id == 0 || move_id > 2000) return;

        for (u32 i = 0; i < move_record_count; i++) {
            MoveScoreRecord& r = move_records[i];
            if (r.move_id == move_id && r.target_low == target_key) {
                if (r.target_species == 0 && target_species != 0) r.target_species = target_species;
                r.score_sum += score_delta;
                if (score_delta < r.score_min) r.score_min = score_delta;
                if (score_delta > r.score_max) r.score_max = score_delta;
                r.count++;
                return;
            }
        }

        if (move_record_count >= 32) return;
        MoveScoreRecord& r = move_records[move_record_count++];
        r.move_id = move_id;
        r.target_low = target_key;
        r.target_species = target_species;
        r.pad = 0;
        r.score_sum = score_delta;
        r.score_min = score_delta;
        r.score_max = score_delta;
        r.count = 1;
    }

    static inline void DumpMoveScoreSummary(u32 hit) {
        if (!global_config.ai_bridge.score_survey_mode) return;
        if (!ShouldLog(hit)) return;
        for (u32 i = 0; i < move_record_count; i++) {
            const MoveScoreRecord& r = move_records[i];
            Logging.Log("[ai_bridge] move_score_summary hit=%u rec=%u move=%u target=%08x target_species=%u/%s sum=%d min=%d max=%d count=%u\n",
                hit, i, r.move_id, r.target_low, static_cast<u32>(r.target_species), KnownSpeciesName(r.target_species),
                r.score_sum, r.score_min, r.score_max, r.count);
        }
    }

    static inline s32 BestMoveSummaryScore() {
        if (move_record_count == 0) return -999999;
        s32 best = -999999;
        for (u32 i = 0; i < move_record_count; i++) {
            const MoveScoreRecord& r = move_records[i];
            if (r.score_sum > best) best = r.score_sum;
        }
        return best;
    }

    static inline s32 WorstMoveSummaryScore() {
        if (move_record_count == 0) return 999999;
        s32 worst = 999999;
        for (u32 i = 0; i < move_record_count; i++) {
            const MoveScoreRecord& r = move_records[i];
            if (r.score_min < worst) worst = r.score_min;
            if (r.score_sum < worst) worst = r.score_sum;
        }
        return worst;
    }

    static inline bool MoveSummaryIncludesTargetSpecies(u16 species) {
        for (u32 i = 0; i < move_record_count; i++) {
            if (move_records[i].target_species == species) return true;
        }
        return false;
    }

    static inline u16 SpeciesFromCandidatePointer(u64 ptr) {
        if (!LooksLikePtr(ptr)) return 0;
        // V14 survey found switch-in Lucario under cs_78 at +0x38,
        // while active/target objects often expose species at +0x70.
        const u16 species_38 = ReadU16(ptr, 0x38);
        if (IsKnownProbeSpecies(species_38)) return species_38;
        const u16 species_70 = ReadU16(ptr, 0x70);
        if (IsKnownProbeSpecies(species_70)) return species_70;
        return 0;
    }

    static inline u16 CandidateSwitchInSpecies(u64 state_ptr) {
        // V14: cs_78 consistently pointed at the switch-in candidate object.
        return SpeciesFromCandidatePointer(LoHiPtr(state_ptr, 0x78));
    }

    static inline u16 CandidateActiveSpecies(u64 state_ptr) {
        // V14: cs_80 tracked the active Pokémon whose switch candidates were
        // being considered: Mienshao first, Gallade later.
        return SpeciesFromCandidatePointer(LoHiPtr(state_ptr, 0x80));
    }



    struct TrainerTeamDef {
        u16 trainer_id;
        u8 count;
        u16 species[6];
    };

    static constexpr TrainerTeamDef kTrainerTeams[] = {
        {2, 3, {833, 835, 846, 0, 0, 0}},
        {3, 2, {819, 0, 831, 0, 0, 0}},
        {7, 4, {831, 821, 816, 835, 0, 0}},
        {8, 3, {831, 821, 0, 835, 0, 0}},
        {9, 4, {831, 821, 813, 835, 0, 0}},
        {10, 3, {824, 827, 821, 0, 0, 0}},
        {12, 1, {0, 0, 0, 828, 0, 0}},
        {13, 2, {0, 0, 83, 0, 760, 0}},
        {14, 3, {510, 820, 0, 0, 184, 0}},
        {15, 3, {315, 830, 763, 0, 0, 0}},
        {16, 3, {851, 825, 0, 0, 752, 0}},
        {17, 2, {0, 822, 164, 0, 0, 0}},
        {18, 2, {0, 185, 0, 0, 95, 0}},
        {19, 1, {0, 0, 0, 0, 838, 0}},
        {22, 3, {860, 0, 38, 0, 836, 0}},
        {23, 2, {863, 0, 12, 0, 0, 0}},
        {24, 3, {310, 0, 750, 0, 743, 0}},
        {25, 2, {26, 0, 0, 0, 26, 0}},
        {26, 3, {0, 737, 214, 0, 768, 0}},
        {28, 1, {0, 0, 0, 0, 869, 0}},
        {29, 3, {547, 549, 0, 0, 830, 0}},
        {30, 3, {0, 315, 0, 711, 709, 0}},
        {31, 3, {763, 0, 754, 0, 781, 0}},
        {32, 6, {407, 272, 275, 841, 842, 830}},
        {36, 5, {279, 847, 845, 0, 350, 834}},
        {37, 4, {38, 59, 0, 0, 663, 851}},
        {38, 2, {695, 0, 310, 0, 0, 0}},
        {39, 2, {0, 777, 82, 0, 0, 0}},
        {40, 5, {573, 763, 750, 745, 214, 0}},
        {41, 4, {841, 842, 869, 855, 0, 0}},
        {42, 4, {760, 743, 282, 475, 0, 0}},
        {43, 4, {59, 0, 745, 745, 745, 0}},
        {44, 1, {0, 0, 0, 615, 0, 0}},
        {45, 2, {0, 0, 195, 0, 746, 0}},
        {46, 1, {99, 0, 0, 0, 0, 0}},
        {47, 2, {0, 130, 226, 0, 0, 0}},
        {48, 3, {534, 675, 0, 0, 701, 0}},
        {49, 4, {839, 0, 558, 874, 526, 0}},
        {50, 4, {530, 208, 0, 195, 344, 0}},
        {51, 1, {242, 0, 0, 0, 0, 0}},
        {52, 2, {715, 0, 849, 0, 0, 0}},
        {53, 1, {0, 866, 0, 0, 0, 0}},
        {54, 2, {0, 0, 73, 0, 758, 0}},
        {55, 2, {0, 0, 675, 534, 0, 0}},
        {56, 3, {0, 560, 760, 68, 0, 0}},
        {57, 3, {0, 185, 0, 864, 865, 0}},
        {59, 4, {547, 591, 841, 842, 0, 0}},
        {60, 2, {475, 38, 0, 0, 0, 0}},
        {61, 2, {282, 59, 0, 0, 0, 0}},
        {62, 5, {620, 475, 534, 448, 675, 0}},
        {63, 3, {620, 0, 534, 865, 0, 0}},
        {64, 3, {560, 214, 0, 0, 784, 0}},
        {65, 3, {709, 711, 781, 0, 0, 0}},
        {66, 3, {593, 0, 478, 426, 0, 0}},
        {67, 5, {563, 477, 770, 681, 623, 0}},
        {68, 2, {876, 0, 0, 242, 0, 0}},
        {69, 5, {663, 823, 630, 628, 227, 0}},
        {70, 5, {858, 876, 407, 706, 478, 0}},
        {72, 3, {880, 0, 882, 780, 0, 0}},
        {73, 4, {777, 738, 0, 849, 871, 0}},
        {74, 2, {530, 0, 448, 0, 0, 0}},
        {75, 1, {0, 863, 0, 0, 0, 0}},
        {76, 4, {836, 745, 59, 745, 0, 0}},
        {77, 6, {701, 534, 865, 870, 853, 68}},
        {78, 6, {867, 609, 864, 778, 855, 94}},
        {79, 1, {0, 699, 0, 0, 0, 0}},
        {80, 4, {0, 526, 530, 834, 689, 0}},
        {81, 2, {0, 565, 0, 0, 558, 0}},
        {82, 1, {0, 0, 0, 0, 185, 0}},
        {83, 2, {0, 460, 0, 0, 478, 0}},
        {84, 1, {38, 0, 0, 0, 0, 0}},
        {85, 5, {38, 615, 584, 614, 362, 0}},
        {86, 3, {0, 713, 0, 881, 883, 0}},
        {87, 2, {0, 334, 182, 0, 0, 0}},
        {88, 1, {560, 0, 0, 0, 0, 0}},
        {89, 4, {272, 0, 847, 423, 423, 0}},
        {92, 4, {561, 678, 678, 518, 0, 0}},
        {93, 3, {0, 844, 208, 0, 750, 0}},
        {94, 5, {526, 464, 689, 839, 874, 0}},
        {96, 5, {823, 94, 738, 330, 584, 0}},
        {97, 2, {0, 344, 477, 0, 0, 0}},
        {98, 4, {876, 0, 879, 870, 853, 0}},
        {99, 3, {715, 310, 695, 0, 0, 0}},
        {100, 1, {777, 0, 0, 0, 0, 0}},
        {101, 2, {630, 0, 0, 0, 687, 0}},
        {102, 3, {560, 625, 0, 675, 0, 0}},
        {103, 2, {0, 510, 0, 359, 0, 0}},
        {104, 4, {727, 861, 0, 877, 635, 0}},
        {105, 3, {861, 302, 635, 0, 0, 0}},
        {106, 1, {0, 0, 862, 0, 0, 0}},
        {107, 4, {861, 0, 625, 687, 0, 862}},
        {108, 3, {468, 0, 0, 0, 685, 869}},
        {113, 1, {0, 303, 0, 0, 0, 0}},
        {114, 4, {282, 38, 743, 184, 0, 0}},
        {115, 5, {547, 858, 861, 778, 707, 0}},
        {121, 6, {832, 822, 817, 836, 830, 851}},
        {122, 5, {832, 822, 0, 836, 851, 834}},
        {123, 6, {832, 822, 814, 836, 830, 834}},
        {124, 6, {823, 847, 818, 851, 832, 830}},
        {125, 5, {823, 812, 0, 851, 847, 832}},
        {126, 5, {823, 847, 0, 815, 832, 830}},
        {127, 6, {823, 847, 818, 851, 830, 832}},
        {128, 5, {823, 812, 0, 851, 847, 832}},
        {129, 5, {823, 847, 0, 815, 830, 832}},
        {130, 6, {823, 847, 851, 830, 832, 818}},
        {131, 5, {823, 0, 851, 847, 832, 812}},
        {132, 5, {823, 847, 0, 830, 832, 815}},
        {133, 4, {0, 0, 858, 876, 579, 282}},
        {134, 4, {0, 876, 468, 282, 0, 858}},
        {135, 4, {0, 464, 530, 0, 745, 839}},
        {136, 4, {699, 873, 875, 0, 866, 0}},
        {137, 4, {0, 625, 675, 687, 862, 0}},
        {138, 6, {727, 861, 630, 560, 877, 625}},
        {139, 3, {591, 407, 0, 0, 781, 0}},
        {143, 4, {478, 758, 0, 350, 763, 0}},
        {144, 4, {526, 330, 844, 884, 0, 0}},
        {145, 6, {727, 630, 560, 877, 625, 861}},
        {149, 6, {887, 815, 9, 3, 681, 6}},
        {156, 6, {832, 822, 818, 836, 830, 851}},
        {157, 6, {832, 822, 812, 836, 851, 834}},
        {158, 6, {832, 822, 815, 836, 830, 834}},
        {159, 2, {862, 0, 359, 0, 0, 0}},
        {160, 2, {828, 510, 0, 0, 0, 0}},
        {175, 5, {437, 0, 530, 448, 598, 879}},
        {176, 1, {0, 0, 0, 0, 560, 0}},
        {177, 5, {620, 701, 214, 870, 853, 0}},
        {178, 4, {279, 130, 350, 0, 593, 0}},
        {179, 1, {0, 0, 80, 0, 0, 0}},
        {180, 1, {0, 0, 0, 0, 226, 0}},
        {181, 5, {768, 423, 748, 537, 752, 0}},
        {182, 4, {279, 164, 178, 83, 0, 0}},
        {183, 4, {876, 609, 855, 0, 681, 0}},
        {184, 4, {596, 637, 873, 743, 0, 0}},
        {185, 2, {0, 303, 212, 0, 0, 0}},
        {186, 2, {468, 707, 0, 0, 0, 0}},
        {187, 3, {0, 871, 211, 771, 0, 0}},
        {189, 6, {887, 818, 9, 3, 681, 6}},
        {190, 6, {812, 887, 9, 3, 681, 6}},
        {191, 6, {832, 822, 817, 836, 830, 851}},
        {192, 5, {832, 822, 0, 836, 851, 834}},
        {193, 6, {832, 822, 814, 836, 830, 834}},
        {194, 6, {864, 853, 874, 887, 839, 143}},
        {195, 1, {0, 0, 857, 0, 0, 0}},
        {196, 5, {510, 560, 877, 0, 359, 860}},
        {197, 4, {831, 821, 816, 835, 0, 0}},
        {198, 3, {831, 821, 0, 835, 0, 0}},
        {199, 4, {831, 821, 813, 835, 0, 0}},
        {201, 1, {827, 0, 0, 0, 0, 0}},
        {202, 6, {823, 847, 818, 851, 830, 832}},
        {203, 5, {823, 812, 0, 851, 847, 832}},
        {204, 5, {823, 847, 0, 815, 830, 832}},
        {205, 2, {0, 828, 0, 0, 359, 0}},
        {206, 4, {510, 860, 0, 675, 302, 0}},
        {207, 3, {828, 862, 0, 0, 560, 0}},
        {208, 2, {510, 0, 0, 675, 0, 0}},
        {210, 4, {279, 847, 350, 0, 0, 834}},
        {211, 6, {701, 534, 865, 870, 853, 68}},
        {212, 6, {864, 609, 867, 778, 855, 94}},
        {213, 5, {0, 706, 880, 330, 882, 884}},
        {214, 6, {823, 847, 818, 851, 830, 832}},
        {215, 5, {823, 812, 0, 851, 847, 832}},
        {216, 5, {823, 847, 0, 815, 830, 832}},
        {221, 2, {0, 0, 865, 628, 0, 0}},
        {222, 3, {630, 864, 579, 0, 0, 0}},
        {223, 2, {768, 0, 625, 0, 0, 0}},
        {224, 2, {437, 870, 0, 0, 0, 0}},
        {225, 3, {832, 823, 818, 0, 0, 0}},
        {226, 3, {832, 823, 812, 0, 0, 0}},
        {227, 3, {832, 823, 815, 0, 0, 0}},
        {231, 4, {0, 858, 876, 468, 282, 0}},
        {232, 2, {0, 0, 628, 865, 0, 0}},
        {233, 3, {630, 864, 579, 0, 0, 0}},
        {234, 6, {823, 832, 818, 851, 830, 889}},
        {235, 6, {823, 832, 818, 851, 830, 888}},
        {236, 5, {823, 812, 0, 851, 832, 889}},
        {237, 5, {823, 812, 0, 851, 832, 888}},
        {238, 5, {823, 832, 0, 815, 830, 889}},
        {239, 5, {823, 832, 0, 815, 830, 888}},
        {240, 2, {0, 0, 0, 857, 282, 0}},
        {241, 1, {0, 0, 0, 868, 0, 0}},
        {245, 3, {547, 617, 0, 0, 448, 0}},
        {246, 1, {825, 0, 0, 0, 0, 0}},
        {248, 6, {861, 727, 630, 560, 625, 877}},
        {249, 6, {887, 815, 9, 3, 681, 6}},
        {250, 6, {887, 818, 9, 3, 681, 6}},
        {251, 6, {812, 887, 9, 3, 681, 6}},
        {252, 6, {823, 889, 851, 830, 832, 818}},
        {253, 6, {823, 832, 851, 830, 888, 818}},
        {254, 5, {823, 889, 0, 851, 832, 812}},
        {255, 5, {823, 0, 851, 832, 888, 812}},
        {256, 5, {823, 889, 0, 830, 832, 815}},
        {257, 5, {823, 832, 0, 830, 888, 815}},
        {258, 6, {547, 272, 830, 407, 842, 841}},
        {259, 4, {279, 847, 350, 0, 0, 834}},
        {260, 5, {663, 324, 38, 59, 0, 851}},
        {261, 6, {701, 534, 865, 870, 853, 68}},
        {262, 6, {864, 609, 867, 778, 855, 94}},
        {264, 4, {0, 464, 530, 0, 745, 839}},
        {265, 4, {38, 873, 866, 699, 0, 0}},
        {266, 4, {861, 687, 0, 625, 0, 862}},
        {267, 5, {0, 330, 706, 880, 882, 884}},
        {268, 6, {727, 630, 877, 560, 625, 861}},
        {269, 4, {0, 876, 468, 282, 0, 858}},
        {270, 1, {0, 143, 0, 0, 0, 0}},
        {271, 6, {596, 212, 743, 826, 752, 637}},
        {272, 6, {663, 663, 468, 178, 227, 823}},
        {273, 3, {0, 130, 847, 0, 0, 99}},
        {274, 2, {0, 526, 0, 0, 0, 874}},
        {275, 5, {0, 475, 214, 448, 675, 870}},
        {276, 4, {591, 0, 94, 0, 73, 849}},
        {277, 6, {477, 563, 302, 426, 623, 867}},
        {278, 4, {38, 0, 0, 460, 478, 866}},
        {279, 3, {0, 530, 0, 208, 0, 844}},
        {280, 5, {547, 711, 549, 0, 709, 830}},
        {281, 3, {0, 871, 0, 26, 0, 836}},
        {282, 3, {0, 0, 0, 635, 706, 887}},
        {283, 4, {437, 707, 0, 598, 0, 530}},
        {284, 3, {637, 324, 0, 0, 0, 839}},
        {285, 5, {863, 625, 589, 212, 632, 0}},
        {286, 2, {303, 530, 0, 0, 0, 0}},
        {287, 4, {707, 0, 208, 598, 227, 0}},
        {289, 6, {547, 272, 830, 407, 841, 842}},
        {304, 2, {0, 813, 816, 0, 0, 0}},
        {305, 6, {812, 818, 815, 3, 9, 6}},
        {306, 4, {727, 260, 0, 0, 730, 724}},
        {312, 6, {823, 847, 818, 851, 832, 830}},
        {313, 5, {823, 812, 0, 851, 847, 832}},
        {314, 5, {823, 847, 0, 815, 832, 830}},
        {315, 1, {825, 0, 0, 0, 0, 0}},
        {316, 1, {848, 0, 0, 0, 0, 0}},
        {317, 1, {825, 0, 0, 0, 0, 0}},
        {318, 2, {849, 0, 80, 0, 0, 0}},
        {319, 1, {826, 0, 0, 0, 0, 0}},
        {320, 2, {849, 0, 0, 0, 0, 80}},
        {321, 1, {0, 0, 852, 0, 0, 0}},
        {324, 4, {549, 758, 0, 242, 0, 9}},
        {325, 4, {758, 0, 242, 0, 184, 3}},
        {326, 4, {549, 758, 0, 242, 0, 9}},
        {327, 4, {758, 0, 242, 0, 184, 3}},
        {328, 4, {620, 560, 0, 853, 784, 0}},
        {329, 4, {620, 560, 0, 853, 784, 0}},
        {330, 2, {878, 0, 0, 0, 0, 822}},
        {339, 2, {620, 784, 0, 0, 0, 0}},
        {340, 2, {620, 784, 0, 0, 0, 0}},
        {341, 2, {303, 0, 858, 0, 0, 0}},
        {342, 3, {877, 560, 861, 0, 0, 0}},
        {343, 3, {681, 887, 6, 0, 0, 0}},
        {344, 3, {324, 59, 851, 0, 0, 0}},
        {345, 3, {768, 748, 834, 0, 0, 0}},
        {346, 2, {0, 849, 862, 0, 0, 0}},
        {347, 3, {477, 864, 94, 0, 0, 0}},
        {348, 3, {330, 706, 884, 0, 0, 0}},
        {349, 3, {701, 870, 68, 0, 0, 0}},
        {350, 2, {865, 437, 0, 0, 0, 0}},
        {351, 3, {832, 889, 818, 0, 0, 0}},
        {352, 3, {832, 888, 818, 0, 0, 0}},
        {353, 3, {832, 889, 812, 0, 0, 0}},
        {354, 3, {832, 888, 812, 0, 0, 0}},
        {355, 3, {832, 889, 815, 0, 0, 0}},
        {356, 3, {832, 888, 815, 0, 0, 0}},
        {357, 1, {866, 0, 0, 0, 0, 0}},
        {358, 1, {0, 0, 839, 0, 0, 0}},
        {359, 1, {80, 0, 0, 0, 0, 0}},
        {360, 1, {0, 0, 80, 0, 0, 0}},
        {361, 3, {863, 306, 879, 0, 0, 0}},
        {362, 3, {865, 768, 625, 0, 0, 0}},
        {363, 2, {272, 0, 842, 0, 0, 0}},
        {364, 2, {0, 468, 869, 0, 0, 0}},
        {365, 2, {275, 0, 841, 0, 0, 0}},
        {372, 4, {620, 560, 0, 853, 784, 0}},
        {373, 4, {620, 560, 0, 853, 784, 0}},
        {374, 3, {0, 475, 0, 0, 687, 826}},
        {375, 3, {0, 475, 0, 0, 687, 826}},
        {376, 5, {591, 80, 0, 748, 758, 849}},
        {377, 5, {591, 80, 0, 748, 758, 849}},
        {378, 6, {887, 815, 9, 3, 681, 6}},
        {379, 6, {887, 818, 9, 3, 681, 6}},
        {380, 6, {812, 887, 9, 3, 681, 6}},
        {381, 6, {823, 889, 851, 830, 832, 818}},
        {382, 6, {823, 832, 851, 830, 888, 818}},
        {383, 5, {823, 889, 0, 851, 832, 812}},
        {384, 5, {823, 0, 851, 832, 888, 812}},
        {385, 5, {823, 889, 0, 830, 832, 815}},
        {386, 5, {823, 832, 0, 830, 888, 815}},
        {387, 6, {547, 272, 830, 407, 842, 841}},
        {388, 4, {279, 847, 350, 0, 0, 834}},
        {389, 5, {663, 324, 38, 59, 0, 851}},
        {390, 6, {701, 534, 865, 870, 853, 68}},
        {391, 6, {864, 609, 867, 778, 855, 94}},
        {392, 4, {0, 464, 530, 0, 745, 839}},
        {393, 4, {38, 873, 866, 699, 0, 0}},
        {394, 4, {861, 687, 0, 625, 0, 862}},
        {395, 5, {0, 330, 706, 880, 882, 884}},
        {396, 6, {727, 630, 877, 560, 625, 861}},
        {397, 4, {0, 876, 468, 282, 0, 858}},
        {398, 1, {0, 143, 0, 0, 0, 0}},
        {399, 6, {596, 212, 743, 826, 752, 637}},
        {400, 6, {663, 663, 468, 178, 227, 823}},
        {401, 3, {0, 130, 847, 0, 0, 99}},
        {402, 2, {0, 526, 0, 0, 0, 874}},
        {403, 5, {0, 475, 214, 448, 675, 870}},
        {404, 4, {591, 0, 94, 0, 73, 849}},
        {405, 6, {477, 563, 302, 426, 623, 867}},
        {406, 4, {38, 0, 0, 460, 478, 866}},
        {407, 3, {0, 530, 0, 208, 0, 844}},
        {408, 5, {547, 711, 549, 0, 709, 830}},
        {409, 3, {0, 871, 0, 26, 0, 836}},
        {410, 3, {0, 0, 0, 635, 706, 887}},
        {411, 4, {437, 707, 0, 598, 0, 530}},
        {412, 3, {637, 324, 0, 0, 0, 839}},
        {413, 6, {547, 272, 830, 407, 841, 842}},
        {414, 1, {825, 0, 0, 0, 0, 0}},
        {415, 1, {848, 0, 0, 0, 0, 0}},
        {416, 1, {852, 0, 0, 0, 0, 0}},
        {417, 1, {825, 0, 0, 0, 0, 0}},
        {418, 1, {848, 0, 0, 0, 0, 0}},
        {419, 1, {825, 0, 0, 0, 0, 0}},
        {420, 2, {848, 0, 0, 0, 0, 80}},
        {431, 6, {861, 727, 630, 560, 625, 877}},
        {432, 4, {620, 0, 560, 784, 853, 0}},
        {433, 4, {620, 0, 560, 784, 853, 0}},
        {434, 4, {823, 437, 879, 0, 0, 598}},
    };

    static inline bool TeamContains(const TrainerTeamDef& team, u16 species) {
        if (species == 0) return true;
        for (u32 i = 0; i < 6; i++) {
            if (team.species[i] == species) return true;
        }
        return false;
    }

    static inline s32 TeamMatchScore(const TrainerTeamDef& team, u16 active, u16 fallback) {
        s32 score = 0;
        if (active != 0 && TeamContains(team, active)) score += 80;
        else if (active != 0) score -= 100;
        if (fallback != 0 && TeamContains(team, fallback)) score += 60;
        // Prefer teams with more known slots; unknown-heavy tables are less reliable.
        for (u32 i = 0; i < 6; i++) if (team.species[i] != 0) score += 2;
        return score;
    }

    static inline const TrainerTeamDef* ResolveTrainerTeam(u64 state_ptr) {
        const u16 active = CandidateActiveSpecies(state_ptr);
        const u16 fallback = CandidateSwitchInSpecies(state_ptr);
        const TrainerTeamDef* best = nullptr;
        s32 best_score = -9999;
        for (u32 i = 0; i < sizeof(kTrainerTeams) / sizeof(kTrainerTeams[0]); i++) {
            const TrainerTeamDef& team = kTrainerTeams[i];
            const s32 score = TeamMatchScore(team, active, fallback);
            if (score > best_score) {
                best_score = score;
                best = &team;
            }
        }
        if (best_score < 60) return nullptr;
        return best;
    }

    static inline bool SpeciesBelongsToResolvedTeam(u64 state_ptr, u16 species) {
        const TrainerTeamDef* team = ResolveTrainerTeam(state_ptr);
        if (!team) return false;
        return TeamContains(*team, species);
    }

    static inline u16 InferredSwitchSpeciesForAction(u64 state_ptr, u32 action_id) {
        const u16 fallback = CandidateSwitchInSpecies(state_ptr);
        const TrainerTeamDef* team = ResolveTrainerTeam(state_ptr);
        if (team && action_id < 6 && team->species[action_id] != 0) {
            return team->species[action_id];
        }
        return fallback;
    }

    static inline void LogResolvedTeam(const char* tag, u64 state_ptr, u32 hit) {
        if (!ShouldLog(hit)) return;
        const TrainerTeamDef* team = ResolveTrainerTeam(state_ptr);
        if (!team) {
            Logging.Log("[ai_bridge] %s v19_team unresolved active=%u/%s fallback=%u/%s\n",
                tag,
                static_cast<u32>(CandidateActiveSpecies(state_ptr)), KnownSpeciesName(CandidateActiveSpecies(state_ptr)),
                static_cast<u32>(CandidateSwitchInSpecies(state_ptr)), KnownSpeciesName(CandidateSwitchInSpecies(state_ptr)));
            return;
        }
        Logging.Log("[ai_bridge] %s v19_team trainer=%u active=%u/%s fallback=%u/%s slots=%u,%u,%u,%u,%u,%u\n",
            tag, static_cast<u32>(team->trainer_id),
            static_cast<u32>(CandidateActiveSpecies(state_ptr)), KnownSpeciesName(CandidateActiveSpecies(state_ptr)),
            static_cast<u32>(CandidateSwitchInSpecies(state_ptr)), KnownSpeciesName(CandidateSwitchInSpecies(state_ptr)),
            static_cast<u32>(team->species[0]), static_cast<u32>(team->species[1]), static_cast<u32>(team->species[2]),
            static_cast<u32>(team->species[3]), static_cast<u32>(team->species[4]), static_cast<u32>(team->species[5]));
    }


    // ---------------------------------------------------------------------
    // V16: broad matchup-aware switch scoring
    // ---------------------------------------------------------------------
    // This is the first "go big" scoring layer.  It uses a real 18-type chart,
    // live runtime species IDs from the candidate/target objects, move-score
    // summaries gathered from the game itself, and safety rules for high-value
    // lead utility like Fake Out.
    //
    // It still remains conservative: if a species is unknown, the scorer does
    // not invent a positive matchup.  Unknowns are treated as neutral/unsafe.

    enum BattleType : u8 {
        BT_NORMAL = 0,
        BT_FIGHTING = 1,
        BT_FLYING = 2,
        BT_POISON = 3,
        BT_GROUND = 4,
        BT_ROCK = 5,
        BT_BUG = 6,
        BT_GHOST = 7,
        BT_STEEL = 8,
        BT_FIRE = 9,
        BT_WATER = 10,
        BT_GRASS = 11,
        BT_ELECTRIC = 12,
        BT_PSYCHIC = 13,
        BT_ICE = 14,
        BT_DRAGON = 15,
        BT_DARK = 16,
        BT_FAIRY = 17,
        BT_NONE = 255
    };

    struct SpeciesTypeInfo {
        u8 t1;
        u8 t2;
    };

    static inline SpeciesTypeInfo MakeTypes(u8 t1, u8 t2 = BT_NONE) {
        SpeciesTypeInfo out = {t1, t2};
        return out;
    }

    static inline bool HasSpeciesTypes(SpeciesTypeInfo t) {
        return t.t1 != BT_NONE;
    }

    static inline bool SpeciesHasType(SpeciesTypeInfo info, u8 type) {
        if (type == BT_NONE) return false;
        return info.t1 == type || info.t2 == type;
    }

    static inline const char* TypeName(u8 type) {
        switch (type) {
            case BT_NORMAL: return "Normal";
            case BT_FIGHTING: return "Fighting";
            case BT_FLYING: return "Flying";
            case BT_POISON: return "Poison";
            case BT_GROUND: return "Ground";
            case BT_ROCK: return "Rock";
            case BT_BUG: return "Bug";
            case BT_GHOST: return "Ghost";
            case BT_STEEL: return "Steel";
            case BT_FIRE: return "Fire";
            case BT_WATER: return "Water";
            case BT_GRASS: return "Grass";
            case BT_ELECTRIC: return "Electric";
            case BT_PSYCHIC: return "Psychic";
            case BT_ICE: return "Ice";
            case BT_DRAGON: return "Dragon";
            case BT_DARK: return "Dark";
            case BT_FAIRY: return "Fairy";
            default: return "?";
        }
    }

    static inline SpeciesTypeInfo SpeciesTypes(u16 species) {
        switch (species) {
            // Current test/player/Ian species and common SwSh/Galar species.
            case 3: return MakeTypes(BT_GRASS, BT_POISON);     // Venusaur
            case 6: return MakeTypes(BT_FIRE, BT_FLYING);      // Charizard
            case 9: return MakeTypes(BT_WATER);                // Blastoise
            case 12: return MakeTypes(BT_BUG, BT_FLYING);      // Butterfree
            case 25: return MakeTypes(BT_ELECTRIC);            // Pikachu
            case 26: return MakeTypes(BT_ELECTRIC);            // Raichu
            case 38: return MakeTypes(BT_FIRE);                // Ninetales
            case 45: return MakeTypes(BT_GRASS, BT_POISON);    // Vileplume
            case 59: return MakeTypes(BT_FIRE);                // Arcanine
            case 68: return MakeTypes(BT_FIGHTING);            // Machamp
            case 73: return MakeTypes(BT_WATER, BT_POISON);    // Tentacruel
            case 75: return MakeTypes(BT_ROCK, BT_GROUND);     // Graveler
            case 80: return MakeTypes(BT_WATER, BT_PSYCHIC);   // Slowbro
            case 82: return MakeTypes(BT_ELECTRIC, BT_STEEL);  // Magneton
            case 83: return MakeTypes(BT_NORMAL, BT_FLYING);   // Farfetch'd
            case 94: return MakeTypes(BT_GHOST, BT_POISON);    // Gengar
            case 95: return MakeTypes(BT_ROCK, BT_GROUND);     // Onix
            case 99: return MakeTypes(BT_WATER);               // Kingler
            case 112: return MakeTypes(BT_GROUND, BT_ROCK);    // Rhydon
            case 130: return MakeTypes(BT_WATER, BT_FLYING);   // Gyarados
            case 143: return MakeTypes(BT_NORMAL);             // Snorlax
            case 164: return MakeTypes(BT_NORMAL, BT_FLYING);  // Noctowl
            case 178: return MakeTypes(BT_PSYCHIC, BT_FLYING); // Xatu
            case 182: return MakeTypes(BT_GRASS);              // Bellossom
            case 184: return MakeTypes(BT_WATER, BT_FAIRY);    // Azumarill
            case 185: return MakeTypes(BT_ROCK);               // Sudowoodo
            case 195: return MakeTypes(BT_WATER, BT_GROUND);   // Quagsire
            case 208: return MakeTypes(BT_STEEL, BT_GROUND);   // Steelix
            case 211: return MakeTypes(BT_WATER, BT_POISON);   // Qwilfish
            case 212: return MakeTypes(BT_BUG, BT_STEEL);      // Scizor
            case 214: return MakeTypes(BT_BUG, BT_FIGHTING);   // Heracross
            case 219: return MakeTypes(BT_FIRE, BT_ROCK);      // Magcargo
            case 226: return MakeTypes(BT_WATER, BT_FLYING);   // Mantine
            case 227: return MakeTypes(BT_STEEL, BT_FLYING);   // Skarmory
            case 242: return MakeTypes(BT_NORMAL);             // Blissey
            case 260: return MakeTypes(BT_WATER, BT_GROUND);   // Swampert
            case 272: return MakeTypes(BT_WATER, BT_GRASS);    // Ludicolo
            case 275: return MakeTypes(BT_GRASS, BT_DARK);     // Shiftry
            case 279: return MakeTypes(BT_WATER, BT_FLYING);   // Pelipper
            case 282: return MakeTypes(BT_PSYCHIC, BT_FAIRY);  // Gardevoir
            case 302: return MakeTypes(BT_DARK, BT_GHOST);     // Sableye
            case 303: return MakeTypes(BT_STEEL, BT_FAIRY);    // Mawile
            case 306: return MakeTypes(BT_STEEL, BT_ROCK);     // Aggron
            case 310: return MakeTypes(BT_ELECTRIC);           // Manectric
            case 315: return MakeTypes(BT_GRASS, BT_POISON);   // Roselia
            case 320: return MakeTypes(BT_WATER);              // Wailmer
            case 321: return MakeTypes(BT_WATER);              // Wailord
            case 324: return MakeTypes(BT_FIRE);               // Torkoal
            case 330: return MakeTypes(BT_GROUND, BT_DRAGON);  // Flygon
            case 334: return MakeTypes(BT_DRAGON, BT_FLYING);  // Altaria
            case 344: return MakeTypes(BT_GROUND, BT_PSYCHIC); // Claydol
            case 350: return MakeTypes(BT_WATER);              // Milotic
            case 359: return MakeTypes(BT_DARK);               // Absol
            case 362: return MakeTypes(BT_ICE);                // Glalie
            case 407: return MakeTypes(BT_GRASS, BT_POISON);   // Roserade
            case 423: return MakeTypes(BT_WATER, BT_GROUND);   // Gastrodon
            case 426: return MakeTypes(BT_GHOST, BT_FLYING);   // Drifblim
            case 437: return MakeTypes(BT_STEEL, BT_PSYCHIC);  // Bronzong
            case 448: return MakeTypes(BT_FIGHTING, BT_STEEL); // Lucario
            case 460: return MakeTypes(BT_GRASS, BT_ICE);      // Abomasnow
            case 464: return MakeTypes(BT_GROUND, BT_ROCK);    // Rhyperior
            case 468: return MakeTypes(BT_FAIRY, BT_FLYING);   // Togekiss
            case 475: return MakeTypes(BT_PSYCHIC, BT_FIGHTING); // Gallade
            case 477: return MakeTypes(BT_GHOST);              // Dusknoir
            case 478: return MakeTypes(BT_ICE, BT_GHOST);      // Froslass
            case 479: return MakeTypes(BT_ELECTRIC, BT_GHOST); // Rotom base
            case 510: return MakeTypes(BT_DARK);               // Liepard
            case 518: return MakeTypes(BT_PSYCHIC);            // Musharna
            case 521: return MakeTypes(BT_NORMAL, BT_FLYING);  // Unfezant
            case 526: return MakeTypes(BT_ROCK);               // Gigalith
            case 530: return MakeTypes(BT_GROUND, BT_STEEL);   // Excadrill
            case 534: return MakeTypes(BT_FIGHTING);           // Conkeldurr
            case 537: return MakeTypes(BT_WATER, BT_GROUND);   // Seismitoad
            case 542: return MakeTypes(BT_BUG, BT_GRASS);      // Leavanny
            case 547: return MakeTypes(BT_GRASS, BT_FAIRY);    // Whimsicott
            case 549: return MakeTypes(BT_GRASS);              // Lilligant
            case 558: return MakeTypes(BT_BUG, BT_ROCK);       // Crustle
            case 560: return MakeTypes(BT_DARK, BT_FIGHTING);  // Scrafty
            case 561: return MakeTypes(BT_PSYCHIC, BT_FLYING); // Sigilyph
            case 563: return MakeTypes(BT_GHOST);              // Cofagrigus
            case 565: return MakeTypes(BT_WATER, BT_ROCK);     // Carracosta
            case 573: return MakeTypes(BT_NORMAL);             // Cinccino
            case 579: return MakeTypes(BT_PSYCHIC);            // Reuniclus
            case 584: return MakeTypes(BT_ICE);                // Vanilluxe
            case 586: return MakeTypes(BT_NORMAL, BT_GRASS);   // Sawsbuck
            case 589: return MakeTypes(BT_BUG, BT_STEEL);      // Escavalier
            case 591: return MakeTypes(BT_GRASS, BT_POISON);   // Amoonguss
            case 593: return MakeTypes(BT_WATER, BT_GHOST);    // Jellicent
            case 596: return MakeTypes(BT_BUG, BT_ELECTRIC);   // Galvantula
            case 598: return MakeTypes(BT_GRASS, BT_STEEL);    // Ferrothorn
            case 609: return MakeTypes(BT_GHOST, BT_FIRE);     // Chandelure
            case 612: return MakeTypes(BT_DRAGON);             // Haxorus
            case 614: return MakeTypes(BT_ICE);                // Beartic
            case 615: return MakeTypes(BT_ICE);                // Cryogonal
            case 617: return MakeTypes(BT_BUG);                // Accelgor
            case 620: return MakeTypes(BT_FIGHTING);           // Mienshao
            case 623: return MakeTypes(BT_GROUND, BT_GHOST);   // Golurk
            case 625: return MakeTypes(BT_DARK, BT_STEEL);     // Bisharp
            case 628: return MakeTypes(BT_NORMAL, BT_FLYING);  // Braviary
            case 630: return MakeTypes(BT_DARK, BT_FLYING);    // Mandibuzz
            case 632: return MakeTypes(BT_BUG, BT_STEEL);      // Durant
            case 635: return MakeTypes(BT_DARK, BT_DRAGON);    // Hydreigon
            case 637: return MakeTypes(BT_BUG, BT_FIRE);       // Volcarona
            case 660: return MakeTypes(BT_NORMAL, BT_GROUND);  // Diggersby
            case 663: return MakeTypes(BT_FIRE, BT_FLYING);    // Talonflame
            case 671: return MakeTypes(BT_FAIRY);              // Florges
            case 675: return MakeTypes(BT_FIGHTING, BT_DARK);  // Pangoro
            case 678: return MakeTypes(BT_PSYCHIC);            // Meowstic
            case 681: return MakeTypes(BT_STEEL, BT_GHOST);    // Aegislash
            case 685: return MakeTypes(BT_FAIRY);              // Slurpuff
            case 687: return MakeTypes(BT_DARK, BT_PSYCHIC);   // Malamar
            case 689: return MakeTypes(BT_ROCK, BT_WATER);     // Barbaracle
            case 695: return MakeTypes(BT_ELECTRIC, BT_NORMAL);// Heliolisk
            case 699: return MakeTypes(BT_ROCK, BT_ICE);       // Aurorus
            case 701: return MakeTypes(BT_FIGHTING, BT_FLYING);// Hawlucha
            case 706: return MakeTypes(BT_DRAGON);             // Goodra
            case 707: return MakeTypes(BT_STEEL, BT_FAIRY);    // Klefki
            case 709: return MakeTypes(BT_GHOST, BT_GRASS);    // Trevenant
            case 711: return MakeTypes(BT_GHOST, BT_GRASS);    // Gourgeist
            case 713: return MakeTypes(BT_ICE);                // Avalugg
            case 715: return MakeTypes(BT_FLYING, BT_DRAGON);  // Noivern
            case 724: return MakeTypes(BT_GRASS, BT_GHOST);    // Decidueye
            case 727: return MakeTypes(BT_FIRE, BT_DARK);      // Incineroar
            case 730: return MakeTypes(BT_WATER, BT_FAIRY);    // Primarina
            case 737: return MakeTypes(BT_BUG, BT_ELECTRIC);   // Charjabug
            case 738: return MakeTypes(BT_BUG, BT_ELECTRIC);   // Vikavolt
            case 740: return MakeTypes(BT_FIGHTING, BT_ICE);   // Crabominable
            case 743: return MakeTypes(BT_BUG, BT_FAIRY);      // Ribombee
            case 745: return MakeTypes(BT_ROCK);               // Lycanroc
            case 746: return MakeTypes(BT_WATER);              // Wishiwashi
            case 748: return MakeTypes(BT_POISON, BT_WATER);   // Toxapex
            case 750: return MakeTypes(BT_GROUND);             // Mudsdale
            case 752: return MakeTypes(BT_WATER, BT_BUG);      // Araquanid
            case 754: return MakeTypes(BT_GRASS);              // Lurantis
            case 758: return MakeTypes(BT_POISON, BT_FIRE);    // Salazzle
            case 760: return MakeTypes(BT_NORMAL, BT_FIGHTING);// Bewear
            case 763: return MakeTypes(BT_GRASS);              // Tsareena
            case 768: return MakeTypes(BT_BUG, BT_WATER);      // Golisopod
            case 770: return MakeTypes(BT_GHOST, BT_GROUND);   // Palossand
            case 771: return MakeTypes(BT_WATER);              // Pyukumuku
            case 773: return MakeTypes(BT_NORMAL);             // Silvally
            case 777: return MakeTypes(BT_ELECTRIC, BT_STEEL); // Togedemaru
            case 778: return MakeTypes(BT_GHOST, BT_FAIRY);    // Mimikyu
            case 780: return MakeTypes(BT_NORMAL, BT_DRAGON);  // Drampa
            case 781: return MakeTypes(BT_GHOST, BT_GRASS);    // Dhelmise
            case 784: return MakeTypes(BT_DRAGON, BT_FIGHTING);// Kommo-o
            case 797: return MakeTypes(BT_STEEL, BT_FLYING);   // Celesteela
            case 800: return MakeTypes(BT_PSYCHIC);            // Necrozma
            // Galar / Gen 8
            case 812: return MakeTypes(BT_GRASS);              // Rillaboom
            case 813: return MakeTypes(BT_FIRE);               // Scorbunny
            case 814: return MakeTypes(BT_FIRE);               // Raboot
            case 815: return MakeTypes(BT_FIRE);               // Cinderace
            case 816: return MakeTypes(BT_WATER);              // Sobble
            case 817: return MakeTypes(BT_WATER);              // Drizzile
            case 818: return MakeTypes(BT_WATER);              // Inteleon
            case 819: return MakeTypes(BT_NORMAL);             // Skwovet
            case 820: return MakeTypes(BT_NORMAL);             // Greedent
            case 821: return MakeTypes(BT_FLYING);             // Rookidee
            case 822: return MakeTypes(BT_FLYING);             // Corvisquire
            case 823: return MakeTypes(BT_FLYING, BT_STEEL);   // Corviknight
            case 824: return MakeTypes(BT_BUG);                // Blipbug
            case 825: return MakeTypes(BT_BUG, BT_PSYCHIC);    // Dottler
            case 826: return MakeTypes(BT_BUG, BT_PSYCHIC);    // Orbeetle
            case 827: return MakeTypes(BT_DARK);               // Nickit
            case 828: return MakeTypes(BT_DARK);               // Thievul
            case 829: return MakeTypes(BT_GRASS);              // Gossifleur
            case 830: return MakeTypes(BT_GRASS);              // Eldegoss
            case 831: return MakeTypes(BT_NORMAL);             // Wooloo
            case 832: return MakeTypes(BT_NORMAL);             // Dubwool
            case 833: return MakeTypes(BT_WATER);              // Chewtle
            case 834: return MakeTypes(BT_WATER, BT_ROCK);     // Drednaw
            case 835: return MakeTypes(BT_ELECTRIC);           // Yamper
            case 836: return MakeTypes(BT_ELECTRIC);           // Boltund
            case 837: return MakeTypes(BT_ROCK);               // Rolycoly
            case 838: return MakeTypes(BT_ROCK, BT_FIRE);      // Carkol
            case 839: return MakeTypes(BT_ROCK, BT_FIRE);      // Coalossal
            case 840: return MakeTypes(BT_GRASS, BT_DRAGON);   // Applin
            case 841: return MakeTypes(BT_GRASS, BT_DRAGON);   // Flapple
            case 842: return MakeTypes(BT_GRASS, BT_DRAGON);   // Appletun
            case 843: return MakeTypes(BT_GROUND);             // Silicobra
            case 844: return MakeTypes(BT_GROUND);             // Sandaconda
            case 845: return MakeTypes(BT_FLYING, BT_WATER);   // Cramorant
            case 846: return MakeTypes(BT_WATER);              // Arrokuda
            case 847: return MakeTypes(BT_WATER);              // Barraskewda
            case 848: return MakeTypes(BT_ELECTRIC, BT_POISON);// Toxel
            case 849: return MakeTypes(BT_ELECTRIC, BT_POISON);// Toxtricity
            case 850: return MakeTypes(BT_FIRE, BT_BUG);       // Sizzlipede
            case 851: return MakeTypes(BT_FIRE, BT_BUG);       // Centiskorch
            case 852: return MakeTypes(BT_FIGHTING);           // Clobbopus
            case 853: return MakeTypes(BT_FIGHTING);           // Grapploct
            case 854: return MakeTypes(BT_GHOST);              // Sinistea
            case 855: return MakeTypes(BT_GHOST);              // Polteageist
            case 856: return MakeTypes(BT_PSYCHIC);            // Hatenna
            case 857: return MakeTypes(BT_PSYCHIC);            // Hattrem
            case 858: return MakeTypes(BT_PSYCHIC, BT_FAIRY);  // Hatterene
            case 859: return MakeTypes(BT_DARK, BT_FAIRY);     // Impidimp
            case 860: return MakeTypes(BT_DARK, BT_FAIRY);     // Morgrem
            case 861: return MakeTypes(BT_DARK, BT_FAIRY);     // Grimmsnarl
            case 862: return MakeTypes(BT_DARK, BT_NORMAL);    // Obstagoon
            case 863: return MakeTypes(BT_STEEL);              // Perrserker
            case 864: return MakeTypes(BT_GHOST);              // Cursola
            case 865: return MakeTypes(BT_FIGHTING);           // Sirfetch'd
            case 866: return MakeTypes(BT_ICE, BT_PSYCHIC);    // Mr. Rime
            case 867: return MakeTypes(BT_GROUND, BT_GHOST);   // Runerigus
            case 868: return MakeTypes(BT_FAIRY);              // Milcery
            case 869: return MakeTypes(BT_FAIRY);              // Alcremie
            case 870: return MakeTypes(BT_FIGHTING);           // Falinks
            case 871: return MakeTypes(BT_ELECTRIC);           // Pincurchin
            case 872: return MakeTypes(BT_ICE, BT_BUG);        // Snom
            case 873: return MakeTypes(BT_ICE, BT_BUG);        // Frosmoth
            case 874: return MakeTypes(BT_ROCK);               // Stonjourner
            case 875: return MakeTypes(BT_ICE);                // Eiscue
            case 876: return MakeTypes(BT_PSYCHIC, BT_NORMAL); // Indeedee
            case 877: return MakeTypes(BT_ELECTRIC, BT_DARK);  // Morpeko
            case 878: return MakeTypes(BT_STEEL);              // Cufant
            case 879: return MakeTypes(BT_STEEL);              // Copperajah
            case 880: return MakeTypes(BT_ELECTRIC, BT_DRAGON);// Dracozolt
            case 881: return MakeTypes(BT_ELECTRIC, BT_ICE);   // Arctozolt
            case 882: return MakeTypes(BT_WATER, BT_DRAGON);   // Dracovish
            case 883: return MakeTypes(BT_WATER, BT_ICE);      // Arctovish
            case 884: return MakeTypes(BT_STEEL, BT_DRAGON);   // Duraludon
            case 885: return MakeTypes(BT_DRAGON, BT_GHOST);   // Dreepy
            case 886: return MakeTypes(BT_DRAGON, BT_GHOST);   // Drakloak
            case 887: return MakeTypes(BT_DRAGON, BT_GHOST);   // Dragapult
            case 888: return MakeTypes(BT_FAIRY);              // Zacian
            case 889: return MakeTypes(BT_FIGHTING);           // Zamazenta
            case 890: return MakeTypes(BT_POISON, BT_DRAGON);  // Eternatus
            default: return MakeTypes(BT_NONE, BT_NONE);
        }
    }

    static inline u8 MoveType(u32 move_id) {
        switch (move_id) {
            case 1: return 0;
            case 2: return 1;
            case 3: return 0;
            case 4: return 0;
            case 5: return 0;
            case 6: return 0;
            case 7: return 9;
            case 8: return 14;
            case 9: return 12;
            case 10: return 0;
            case 11: return 0;
            case 12: return 0;
            case 13: return 0;
            case 14: return 0;
            case 15: return 0;
            case 16: return 2;
            case 17: return 2;
            case 18: return 0;
            case 19: return 2;
            case 20: return 0;
            case 21: return 0;
            case 22: return 11;
            case 23: return 0;
            case 24: return 1;
            case 25: return 0;
            case 26: return 1;
            case 27: return 1;
            case 28: return 4;
            case 29: return 0;
            case 30: return 0;
            case 31: return 0;
            case 32: return 0;
            case 33: return 0;
            case 34: return 0;
            case 35: return 0;
            case 36: return 0;
            case 37: return 0;
            case 38: return 0;
            case 39: return 0;
            case 40: return 3;
            case 41: return 6;
            case 42: return 6;
            case 43: return 0;
            case 44: return 16;
            case 45: return 0;
            case 46: return 0;
            case 47: return 0;
            case 48: return 0;
            case 49: return 0;
            case 50: return 0;
            case 51: return 3;
            case 52: return 9;
            case 53: return 9;
            case 54: return 14;
            case 55: return 10;
            case 56: return 10;
            case 57: return 10;
            case 58: return 14;
            case 59: return 14;
            case 60: return 13;
            case 61: return 10;
            case 62: return 14;
            case 63: return 0;
            case 64: return 2;
            case 65: return 2;
            case 66: return 1;
            case 67: return 1;
            case 68: return 1;
            case 69: return 1;
            case 70: return 0;
            case 71: return 11;
            case 72: return 11;
            case 73: return 11;
            case 74: return 0;
            case 75: return 11;
            case 76: return 11;
            case 77: return 3;
            case 78: return 11;
            case 79: return 11;
            case 80: return 11;
            case 81: return 6;
            case 82: return 15;
            case 83: return 9;
            case 84: return 12;
            case 85: return 12;
            case 86: return 12;
            case 87: return 12;
            case 88: return 5;
            case 89: return 4;
            case 90: return 4;
            case 91: return 4;
            case 92: return 3;
            case 93: return 13;
            case 94: return 13;
            case 95: return 13;
            case 96: return 13;
            case 97: return 13;
            case 98: return 0;
            case 99: return 0;
            case 100: return 13;
            case 101: return 7;
            case 102: return 0;
            case 103: return 0;
            case 104: return 0;
            case 105: return 0;
            case 106: return 0;
            case 107: return 0;
            case 108: return 0;
            case 109: return 7;
            case 110: return 10;
            case 111: return 0;
            case 112: return 13;
            case 113: return 13;
            case 114: return 14;
            case 115: return 13;
            case 116: return 0;
            case 117: return 0;
            case 118: return 0;
            case 119: return 2;
            case 120: return 0;
            case 121: return 0;
            case 122: return 7;
            case 123: return 3;
            case 124: return 3;
            case 125: return 4;
            case 126: return 9;
            case 127: return 10;
            case 128: return 10;
            case 129: return 0;
            case 130: return 0;
            case 131: return 0;
            case 132: return 0;
            case 133: return 13;
            case 134: return 13;
            case 135: return 0;
            case 136: return 1;
            case 137: return 0;
            case 138: return 13;
            case 139: return 3;
            case 140: return 0;
            case 141: return 6;
            case 142: return 0;
            case 143: return 2;
            case 144: return 0;
            case 145: return 10;
            case 146: return 0;
            case 147: return 11;
            case 148: return 0;
            case 149: return 13;
            case 150: return 0;
            case 151: return 3;
            case 152: return 10;
            case 153: return 0;
            case 154: return 0;
            case 155: return 4;
            case 156: return 13;
            case 157: return 5;
            case 158: return 0;
            case 159: return 0;
            case 160: return 0;
            case 161: return 0;
            case 162: return 0;
            case 163: return 0;
            case 164: return 0;
            case 165: return 0;
            case 166: return 0;
            case 167: return 1;
            case 168: return 16;
            case 169: return 6;
            case 170: return 0;
            case 171: return 7;
            case 172: return 9;
            case 173: return 0;
            case 174: return 7;
            case 175: return 0;
            case 176: return 0;
            case 177: return 2;
            case 178: return 11;
            case 179: return 1;
            case 180: return 7;
            case 181: return 14;
            case 182: return 0;
            case 183: return 1;
            case 184: return 0;
            case 185: return 16;
            case 186: return 17;
            case 187: return 0;
            case 188: return 3;
            case 189: return 4;
            case 190: return 10;
            case 191: return 4;
            case 192: return 12;
            case 193: return 0;
            case 194: return 7;
            case 195: return 0;
            case 196: return 14;
            case 197: return 1;
            case 198: return 4;
            case 199: return 0;
            case 200: return 15;
            case 201: return 5;
            case 202: return 11;
            case 203: return 0;
            case 204: return 17;
            case 205: return 5;
            case 206: return 0;
            case 207: return 0;
            case 208: return 0;
            case 209: return 12;
            case 210: return 6;
            case 211: return 8;
            case 212: return 0;
            case 213: return 0;
            case 214: return 0;
            case 215: return 0;
            case 216: return 0;
            case 217: return 0;
            case 218: return 0;
            case 219: return 0;
            case 220: return 0;
            case 221: return 9;
            case 222: return 4;
            case 223: return 1;
            case 224: return 6;
            case 225: return 15;
            case 226: return 0;
            case 227: return 0;
            case 228: return 16;
            case 229: return 0;
            case 230: return 0;
            case 231: return 8;
            case 232: return 8;
            case 233: return 1;
            case 234: return 0;
            case 235: return 11;
            case 236: return 17;
            case 237: return 0;
            case 238: return 1;
            case 239: return 15;
            case 240: return 10;
            case 241: return 9;
            case 242: return 16;
            case 243: return 13;
            case 244: return 0;
            case 245: return 0;
            case 246: return 5;
            case 247: return 7;
            case 248: return 13;
            case 249: return 1;
            case 250: return 10;
            case 251: return 16;
            case 252: return 0;
            case 253: return 0;
            case 254: return 0;
            case 255: return 0;
            case 256: return 0;
            case 257: return 9;
            case 258: return 14;
            case 259: return 16;
            case 260: return 16;
            case 261: return 9;
            case 262: return 16;
            case 263: return 0;
            case 264: return 1;
            case 265: return 0;
            case 266: return 0;
            case 267: return 0;
            case 268: return 12;
            case 269: return 16;
            case 270: return 0;
            case 271: return 13;
            case 272: return 13;
            case 273: return 0;
            case 274: return 0;
            case 275: return 11;
            case 276: return 1;
            case 277: return 13;
            case 278: return 0;
            case 279: return 1;
            case 280: return 1;
            case 281: return 0;
            case 282: return 16;
            case 283: return 0;
            case 284: return 9;
            case 285: return 13;
            case 286: return 13;
            case 287: return 0;
            case 288: return 7;
            case 289: return 16;
            case 290: return 0;
            case 291: return 10;
            case 292: return 1;
            case 293: return 0;
            case 294: return 6;
            case 295: return 13;
            case 296: return 13;
            case 297: return 2;
            case 298: return 0;
            case 299: return 9;
            case 300: return 4;
            case 301: return 14;
            case 302: return 11;
            case 303: return 0;
            case 304: return 0;
            case 305: return 3;
            case 306: return 0;
            case 307: return 9;
            case 308: return 10;
            case 309: return 8;
            case 310: return 7;
            case 311: return 0;
            case 312: return 11;
            case 313: return 16;
            case 314: return 2;
            case 315: return 9;
            case 316: return 0;
            case 317: return 5;
            case 318: return 6;
            case 319: return 8;
            case 320: return 11;
            case 321: return 0;
            case 322: return 13;
            case 323: return 10;
            case 324: return 6;
            case 325: return 7;
            case 326: return 13;
            case 327: return 1;
            case 328: return 4;
            case 329: return 14;
            case 330: return 10;
            case 331: return 11;
            case 332: return 2;
            case 333: return 14;
            case 334: return 8;
            case 335: return 0;
            case 336: return 0;
            case 337: return 15;
            case 338: return 11;
            case 339: return 1;
            case 340: return 2;
            case 341: return 4;
            case 342: return 3;
            case 343: return 0;
            case 344: return 12;
            case 345: return 11;
            case 346: return 10;
            case 347: return 13;
            case 348: return 11;
            case 349: return 15;
            case 350: return 5;
            case 351: return 12;
            case 352: return 10;
            case 353: return 8;
            case 354: return 13;
            case 355: return 2;
            case 356: return 13;
            case 357: return 13;
            case 358: return 1;
            case 359: return 1;
            case 360: return 8;
            case 361: return 13;
            case 362: return 10;
            case 363: return 0;
            case 364: return 0;
            case 365: return 2;
            case 366: return 2;
            case 367: return 0;
            case 368: return 8;
            case 369: return 6;
            case 370: return 1;
            case 371: return 16;
            case 372: return 16;
            case 373: return 16;
            case 374: return 16;
            case 375: return 13;
            case 376: return 0;
            case 377: return 13;
            case 378: return 0;
            case 379: return 13;
            case 380: return 3;
            case 381: return 0;
            case 382: return 0;
            case 383: return 0;
            case 384: return 13;
            case 385: return 13;
            case 386: return 16;
            case 387: return 0;
            case 388: return 11;
            case 389: return 16;
            case 390: return 3;
            case 391: return 13;
            case 392: return 10;
            case 393: return 12;
            case 394: return 9;
            case 395: return 1;
            case 396: return 1;
            case 397: return 5;
            case 398: return 3;
            case 399: return 16;
            case 400: return 16;
            case 401: return 10;
            case 402: return 11;
            case 403: return 2;
            case 404: return 6;
            case 405: return 6;
            case 406: return 15;
            case 407: return 15;
            case 408: return 5;
            case 409: return 1;
            case 410: return 1;
            case 411: return 1;
            case 412: return 11;
            case 413: return 2;
            case 414: return 4;
            case 415: return 16;
            case 416: return 0;
            case 417: return 16;
            case 418: return 8;
            case 419: return 14;
            case 420: return 14;
            case 421: return 7;
            case 422: return 12;
            case 423: return 14;
            case 424: return 9;
            case 425: return 7;
            case 426: return 4;
            case 427: return 13;
            case 428: return 13;
            case 429: return 8;
            case 430: return 8;
            case 431: return 0;
            case 432: return 2;
            case 433: return 13;
            case 434: return 15;
            case 435: return 12;
            case 436: return 9;
            case 437: return 11;
            case 438: return 11;
            case 439: return 5;
            case 440: return 3;
            case 441: return 3;
            case 442: return 8;
            case 443: return 8;
            case 444: return 5;
            case 445: return 0;
            case 446: return 5;
            case 447: return 11;
            case 448: return 2;
            case 449: return 0;
            case 450: return 6;
            case 451: return 12;
            case 452: return 11;
            case 453: return 10;
            case 454: return 6;
            case 455: return 6;
            case 456: return 6;
            case 457: return 5;
            case 458: return 0;
            case 459: return 15;
            case 460: return 15;
            case 461: return 13;
            case 462: return 0;
            case 463: return 9;
            case 464: return 16;
            case 465: return 11;
            case 466: return 7;
            case 467: return 7;
            case 468: return 16;
            case 469: return 5;
            case 470: return 13;
            case 471: return 13;
            case 472: return 13;
            case 473: return 13;
            case 474: return 3;
            case 475: return 8;
            case 476: return 6;
            case 477: return 13;
            case 478: return 13;
            case 479: return 5;
            case 480: return 1;
            case 481: return 9;
            case 482: return 3;
            case 483: return 6;
            case 484: return 8;
            case 485: return 13;
            case 486: return 12;
            case 487: return 10;
            case 488: return 9;
            case 489: return 3;
            case 490: return 1;
            case 491: return 3;
            case 492: return 16;
            case 493: return 0;
            case 494: return 0;
            case 495: return 0;
            case 496: return 0;
            case 497: return 0;
            case 498: return 0;
            case 499: return 3;
            case 500: return 13;
            case 501: return 1;
            case 502: return 13;
            case 503: return 10;
            case 504: return 0;
            case 505: return 13;
            case 506: return 7;
            case 507: return 2;
            case 508: return 8;
            case 509: return 1;
            case 510: return 9;
            case 511: return 16;
            case 512: return 2;
            case 513: return 0;
            case 514: return 0;
            case 515: return 1;
            case 516: return 0;
            case 517: return 9;
            case 518: return 10;
            case 519: return 9;
            case 520: return 11;
            case 521: return 12;
            case 522: return 6;
            case 523: return 4;
            case 524: return 14;
            case 525: return 15;
            case 526: return 0;
            case 527: return 12;
            case 528: return 12;
            case 529: return 4;
            case 530: return 15;
            case 531: return 13;
            case 532: return 11;
            case 533: return 1;
            case 534: return 10;
            case 535: return 9;
            case 536: return 11;
            case 537: return 6;
            case 538: return 11;
            case 539: return 16;
            case 540: return 13;
            case 541: return 0;
            case 542: return 2;
            case 543: return 0;
            case 544: return 8;
            case 545: return 9;
            case 546: return 0;
            case 547: return 0;
            case 548: return 1;
            case 549: return 14;
            case 550: return 12;
            case 551: return 9;
            case 552: return 9;
            case 553: return 14;
            case 554: return 14;
            case 555: return 16;
            case 556: return 14;
            case 557: return 9;
            case 558: return 9;
            case 559: return 12;
            case 560: return 1;
            case 561: return 1;
            case 562: return 3;
            case 563: return 4;
            case 564: return 6;
            case 565: return 6;
            case 566: return 7;
            case 567: return 7;
            case 568: return 0;
            case 569: return 12;
            case 570: return 12;
            case 571: return 11;
            case 572: return 11;
            case 573: return 14;
            case 574: return 17;
            case 575: return 16;
            case 576: return 16;
            case 577: return 17;
            case 578: return 17;
            case 579: return 17;
            case 580: return 11;
            case 581: return 17;
            case 582: return 12;
            case 583: return 17;
            case 584: return 17;
            case 585: return 17;
            case 586: return 0;
            case 587: return 17;
            case 588: return 8;
            case 589: return 0;
            case 590: return 0;
            case 591: return 5;
            case 592: return 10;
            case 593: return 13;
            case 594: return 10;
            case 595: return 9;
            case 596: return 11;
            case 597: return 17;
            case 598: return 12;
            case 599: return 3;
            case 600: return 6;
            case 601: return 17;
            case 602: return 12;
            case 603: return 0;
            case 604: return 12;
            case 605: return 17;
            case 606: return 0;
            case 607: return 0;
            case 608: return 17;
            case 609: return 12;
            case 610: return 0;
            case 611: return 6;
            case 612: return 1;
            case 613: return 2;
            case 614: return 4;
            case 615: return 4;
            case 616: return 4;
            case 617: return 17;
            case 618: return 10;
            case 619: return 4;
            case 620: return 2;
            case 621: return 16;
            case 622: return 0;
            case 623: return 0;
            case 624: return 1;
            case 625: return 1;
            case 626: return 2;
            case 627: return 2;
            case 628: return 3;
            case 629: return 3;
            case 630: return 4;
            case 631: return 4;
            case 632: return 5;
            case 633: return 5;
            case 634: return 6;
            case 635: return 6;
            case 636: return 7;
            case 637: return 7;
            case 638: return 8;
            case 639: return 8;
            case 640: return 9;
            case 641: return 9;
            case 642: return 10;
            case 643: return 10;
            case 644: return 11;
            case 645: return 11;
            case 646: return 12;
            case 647: return 12;
            case 648: return 13;
            case 649: return 13;
            case 650: return 14;
            case 651: return 14;
            case 652: return 15;
            case 653: return 15;
            case 654: return 16;
            case 655: return 16;
            case 656: return 17;
            case 657: return 17;
            case 658: return 12;
            case 659: return 4;
            case 660: return 6;
            case 661: return 3;
            case 662: return 7;
            case 663: return 16;
            case 664: return 10;
            case 665: return 14;
            case 666: return 17;
            case 667: return 4;
            case 668: return 11;
            case 669: return 11;
            case 670: return 11;
            case 671: return 0;
            case 672: return 3;
            case 673: return 0;
            case 674: return 8;
            case 675: return 16;
            case 676: return 6;
            case 677: return 8;
            case 678: return 13;
            case 679: return 6;
            case 680: return 9;
            case 681: return 16;
            case 682: return 9;
            case 683: return 13;
            case 684: return 8;
            case 685: return 3;
            case 686: return 0;
            case 687: return 15;
            case 688: return 11;
            case 689: return 13;
            case 690: return 2;
            case 691: return 15;
            case 692: return 15;
            case 693: return 16;
            case 694: return 14;
            case 695: return 7;
            case 696: return 16;
            case 697: return 10;
            case 698: return 17;
            case 699: return 7;
            case 700: return 12;
            case 701: return 0;
            case 702: return 0;
            case 703: return 13;
            case 704: return 9;
            case 705: return 17;
            case 706: return 13;
            case 707: return 4;
            case 708: return 7;
            case 709: return 5;
            case 710: return 10;
            case 711: return 13;
            case 712: return 7;
            case 713: return 8;
            case 714: return 7;
            case 715: return 0;
            case 716: return 12;
            case 717: return 17;
            case 718: return 0;
            case 719: return 12;
            case 720: return 9;
            case 721: return 12;
            case 722: return 13;
            case 723: return 13;
            case 724: return 8;
            case 725: return 7;
            case 726: return 17;
            case 727: return 5;
            case 728: return 15;
            case 729: return 12;
            case 730: return 10;
            case 731: return 2;
            case 732: return 12;
            case 733: return 10;
            case 734: return 12;
            case 735: return 9;
            case 736: return 13;
            case 737: return 16;
            case 738: return 11;
            case 739: return 14;
            case 740: return 17;
            case 741: return 0;
            case 742: return 8;
            case 743: return 0;
            case 744: return 15;
            case 745: return 10;
            case 746: return 16;
            case 747: return 0;
            case 748: return 1;
            case 749: return 5;
            case 750: return 13;
            case 751: return 15;
            case 752: return 0;
            case 753: return 1;
            case 754: return 12;
            case 755: return 10;
            case 756: return 0;
            case 757: return 9;
            case 758: return 6;
            case 759: return 12;
            case 760: return 0;
            case 761: return 1;
            case 762: return 7;
            case 763: return 14;
            case 764: return 3;
            case 765: return 10;
            case 766: return 2;
            case 767: return 17;
            case 768: return 15;
            case 769: return 13;
            case 770: return 5;
            case 771: return 4;
            case 772: return 16;
            case 773: return 11;
            case 774: return 8;
            case 775: return 15;
            case 776: return 1;
            case 777: return 17;
            case 778: return 11;
            case 779: return 11;
            case 780: return 9;
            case 781: return 8;
            case 782: return 8;
            case 783: return 12;
            case 784: return 15;
            case 785: return 11;
            case 786: return 12;
            case 787: return 11;
            case 788: return 11;
            case 789: return 17;
            case 790: return 17;
            case 791: return 10;
            case 792: return 16;
            case 793: return 16;
            case 794: return 1;
            case 795: return 15;
            case 796: return 8;
            case 797: return 13;
            case 798: return 8;
            case 799: return 15;
            case 800: return 5;
            case 801: return 3;
            case 802: return 17;
            case 803: return 11;
            case 804: return 12;
            case 805: return 0;
            case 806: return 6;
            case 807: return 9;
            case 808: return 16;
            case 809: return 7;
            case 810: return 3;
            case 811: return 1;
            case 812: return 10;
            case 813: return 14;
            case 814: return 2;
            case 815: return 4;
            case 816: return 11;
            case 817: return 16;
            case 818: return 10;
            case 819: return 12;
            case 820: return 15;
            case 821: return 13;
            case 822: return 16;
            case 823: return 1;
            case 824: return 14;
            case 825: return 7;
            case 826: return 13;
            default: return BT_NONE;
        }
    }

    static inline u32 TypeEffectiveness10Single(u8 attack, u8 defend) {
        if (attack == BT_NONE || defend == BT_NONE) return 10;
        switch (attack) {
            case BT_NORMAL:
                if (defend == BT_GHOST) return 0;
                if (defend == BT_ROCK || defend == BT_STEEL) return 5;
                return 10;
            case BT_FIGHTING:
                if (defend == BT_GHOST) return 0;
                if (defend == BT_NORMAL || defend == BT_ICE || defend == BT_ROCK || defend == BT_DARK || defend == BT_STEEL) return 20;
                if (defend == BT_POISON || defend == BT_FLYING || defend == BT_PSYCHIC || defend == BT_BUG || defend == BT_FAIRY) return 5;
                return 10;
            case BT_FLYING:
                if (defend == BT_GRASS || defend == BT_FIGHTING || defend == BT_BUG) return 20;
                if (defend == BT_ELECTRIC || defend == BT_ROCK || defend == BT_STEEL) return 5;
                return 10;
            case BT_POISON:
                if (defend == BT_STEEL) return 0;
                if (defend == BT_GRASS || defend == BT_FAIRY) return 20;
                if (defend == BT_POISON || defend == BT_GROUND || defend == BT_ROCK || defend == BT_GHOST) return 5;
                return 10;
            case BT_GROUND:
                if (defend == BT_FLYING) return 0;
                if (defend == BT_FIRE || defend == BT_ELECTRIC || defend == BT_POISON || defend == BT_ROCK || defend == BT_STEEL) return 20;
                if (defend == BT_GRASS || defend == BT_BUG) return 5;
                return 10;
            case BT_ROCK:
                if (defend == BT_FIRE || defend == BT_ICE || defend == BT_FLYING || defend == BT_BUG) return 20;
                if (defend == BT_FIGHTING || defend == BT_GROUND || defend == BT_STEEL) return 5;
                return 10;
            case BT_BUG:
                if (defend == BT_GRASS || defend == BT_PSYCHIC || defend == BT_DARK) return 20;
                if (defend == BT_FIRE || defend == BT_FIGHTING || defend == BT_POISON || defend == BT_FLYING || defend == BT_GHOST || defend == BT_STEEL || defend == BT_FAIRY) return 5;
                return 10;
            case BT_GHOST:
                if (defend == BT_NORMAL) return 0;
                if (defend == BT_PSYCHIC || defend == BT_GHOST) return 20;
                if (defend == BT_DARK) return 5;
                return 10;
            case BT_STEEL:
                if (defend == BT_ICE || defend == BT_ROCK || defend == BT_FAIRY) return 20;
                if (defend == BT_FIRE || defend == BT_WATER || defend == BT_ELECTRIC || defend == BT_STEEL) return 5;
                return 10;
            case BT_FIRE:
                if (defend == BT_GRASS || defend == BT_ICE || defend == BT_BUG || defend == BT_STEEL) return 20;
                if (defend == BT_FIRE || defend == BT_WATER || defend == BT_ROCK || defend == BT_DRAGON) return 5;
                return 10;
            case BT_WATER:
                if (defend == BT_FIRE || defend == BT_GROUND || defend == BT_ROCK) return 20;
                if (defend == BT_WATER || defend == BT_GRASS || defend == BT_DRAGON) return 5;
                return 10;
            case BT_GRASS:
                if (defend == BT_WATER || defend == BT_GROUND || defend == BT_ROCK) return 20;
                if (defend == BT_FIRE || defend == BT_GRASS || defend == BT_POISON || defend == BT_FLYING || defend == BT_BUG || defend == BT_DRAGON || defend == BT_STEEL) return 5;
                return 10;
            case BT_ELECTRIC:
                if (defend == BT_GROUND) return 0;
                if (defend == BT_WATER || defend == BT_FLYING) return 20;
                if (defend == BT_ELECTRIC || defend == BT_GRASS || defend == BT_DRAGON) return 5;
                return 10;
            case BT_PSYCHIC:
                if (defend == BT_DARK) return 0;
                if (defend == BT_FIGHTING || defend == BT_POISON) return 20;
                if (defend == BT_PSYCHIC || defend == BT_STEEL) return 5;
                return 10;
            case BT_ICE:
                if (defend == BT_GRASS || defend == BT_GROUND || defend == BT_FLYING || defend == BT_DRAGON) return 20;
                if (defend == BT_FIRE || defend == BT_WATER || defend == BT_ICE || defend == BT_STEEL) return 5;
                return 10;
            case BT_DRAGON:
                if (defend == BT_FAIRY) return 0;
                if (defend == BT_DRAGON) return 20;
                if (defend == BT_STEEL) return 5;
                return 10;
            case BT_DARK:
                if (defend == BT_PSYCHIC || defend == BT_GHOST) return 20;
                if (defend == BT_FIGHTING || defend == BT_DARK || defend == BT_FAIRY) return 5;
                return 10;
            case BT_FAIRY:
                if (defend == BT_FIGHTING || defend == BT_DRAGON || defend == BT_DARK) return 20;
                if (defend == BT_FIRE || defend == BT_POISON || defend == BT_STEEL) return 5;
                return 10;
            default:
                return 10;
        }
    }

    static inline u32 TypeEffectiveness10(u8 attack, SpeciesTypeInfo defender) {
        if (!HasSpeciesTypes(defender) || attack == BT_NONE) return 10;
        u32 value = TypeEffectiveness10Single(attack, defender.t1);
        if (defender.t2 != BT_NONE && defender.t2 != defender.t1) {
            value = (value * TypeEffectiveness10Single(attack, defender.t2)) / 10;
        }
        return value;
    }

    static inline u32 MaxStabEffectiveness10(SpeciesTypeInfo attacker, SpeciesTypeInfo defender) {
        if (!HasSpeciesTypes(attacker) || !HasSpeciesTypes(defender)) return 10;
        u32 best = TypeEffectiveness10(attacker.t1, defender);
        if (attacker.t2 != BT_NONE && attacker.t2 != attacker.t1) {
            const u32 second = TypeEffectiveness10(attacker.t2, defender);
            if (second > best) best = second;
        }
        return best;
    }


    static inline bool AddThreatMoveType(u8* out_types, u32* out_count, u8 type) {
        if (type == BT_NONE) return false;
        for (u32 i = 0; i < *out_count; i++) {
            if (out_types[i] == type) return false;
        }
        if (*out_count >= 8) return false;
        out_types[*out_count] = type;
        (*out_count)++;
        return true;
    }

    static inline u32 SpeciesThreatMoveProfile(u16 species, u8* out_types) {
        u32 count = 0;
        const SpeciesTypeInfo base = SpeciesTypes(species);
        if (base.t1 != BT_NONE) AddThreatMoveType(out_types, &count, base.t1);
        if (base.t2 != BT_NONE && base.t2 != base.t1) AddThreatMoveType(out_types, &count, base.t2);

        // V19: threat profiles for ability/coverage-heavy Pokémon.
        // We evaluate what the target can plausibly click next, not only what type it currently is.
        // This is especially important for Libero/Protean users.  A Cinderace that used Protect
        // and temporarily became Normal-type can still click Pyro Ball next turn, so Lucario should
        // not be treated as a safe switch merely because it resists Normal.
        switch (species) {
            case 815: // Cinderace / Libero. Include observed user moves and common coverage.
                AddThreatMoveType(out_types, &count, BT_FIRE);     // Pyro Ball / Fire STAB
                AddThreatMoveType(out_types, &count, BT_STEEL);    // Iron Head, observed
                AddThreatMoveType(out_types, &count, BT_NORMAL);   // Feint / Protect-type bait
                AddThreatMoveType(out_types, &count, BT_FIGHTING); // HJK/Low Kick style coverage
                AddThreatMoveType(out_types, &count, BT_DARK);     // Sucker Punch / Dark coverage
                break;
            case 658: // Greninja / Protean-style in transfer/modded contexts.
                AddThreatMoveType(out_types, &count, BT_WATER);
                AddThreatMoveType(out_types, &count, BT_DARK);
                AddThreatMoveType(out_types, &count, BT_ICE);
                AddThreatMoveType(out_types, &count, BT_POISON);
                AddThreatMoveType(out_types, &count, BT_GRASS);
                break;
            case 861: // Grimmsnarl.
                AddThreatMoveType(out_types, &count, BT_FAIRY);
                AddThreatMoveType(out_types, &count, BT_DARK);
                break;
            case 812: // Rillaboom.
                AddThreatMoveType(out_types, &count, BT_GRASS);
                AddThreatMoveType(out_types, &count, BT_GROUND);
                AddThreatMoveType(out_types, &count, BT_DARK);
                break;
            case 823: // Corviknight.
                AddThreatMoveType(out_types, &count, BT_FLYING);
                AddThreatMoveType(out_types, &count, BT_STEEL);
                break;
            case 887: // Dragapult.
                AddThreatMoveType(out_types, &count, BT_DRAGON);
                AddThreatMoveType(out_types, &count, BT_GHOST);
                AddThreatMoveType(out_types, &count, BT_FIRE);
                AddThreatMoveType(out_types, &count, BT_ELECTRIC);
                break;
            case 858: // Hatterene.
                AddThreatMoveType(out_types, &count, BT_FAIRY);
                AddThreatMoveType(out_types, &count, BT_PSYCHIC);
                AddThreatMoveType(out_types, &count, BT_FIRE);
                AddThreatMoveType(out_types, &count, BT_GRASS);
                break;
            case 834: // Drednaw.
                AddThreatMoveType(out_types, &count, BT_WATER);
                AddThreatMoveType(out_types, &count, BT_ROCK);
                AddThreatMoveType(out_types, &count, BT_GROUND);
                break;
            case 836: // Boltund.
                AddThreatMoveType(out_types, &count, BT_ELECTRIC);
                AddThreatMoveType(out_types, &count, BT_DARK);
                AddThreatMoveType(out_types, &count, BT_FIRE);
                break;
            default:
                break;
        }
        return count;
    }

    static inline bool SpeciesUsesMoveTypeThreatLogic(u16 species) {
        return species == 815 || species == 658; // Cinderace/Libero, Greninja/Protean-style.
    }

    static inline u32 MaxThreatMoveEffectiveness10(u16 attacker_species, SpeciesTypeInfo defender) {
        if (!HasSpeciesTypes(defender)) return 10;
        u8 threat_types[8] = {BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE};
        const u32 count = SpeciesThreatMoveProfile(attacker_species, threat_types);
        if (count == 0) return 10;
        u32 best = 0;
        for (u32 i = 0; i < count; i++) {
            const u32 eff = TypeEffectiveness10(threat_types[i], defender);
            if (eff > best) best = eff;
        }
        return best == 0 ? 10 : best;
    }

    static inline u32 MaxSwitchOffenseEffectiveness10(u16 switch_species, SpeciesTypeInfo defender) {
        if (!HasSpeciesTypes(defender)) return 10;
        // Offense should also use plausible coverage if we know it, but STAB remains the fallback.
        u32 best = MaxStabEffectiveness10(SpeciesTypes(switch_species), defender);
        u8 threat_types[8] = {BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE, BT_NONE};
        const u32 count = SpeciesThreatMoveProfile(switch_species, threat_types);
        for (u32 i = 0; i < count; i++) {
            const u32 eff = TypeEffectiveness10(threat_types[i], defender);
            if (eff > best) best = eff;
        }
        return best;
    }

    static inline bool IsKnownFriendlyCandidateSpeciesForState(u64 state_ptr, u16 species, u16 active_species, u16 switch_species) {
        if (species == 0) return true;
        if (species == active_species || species == switch_species) return true;
        if (SpeciesBelongsToResolvedTeam(state_ptr, species)) return true;
        return false;
    }

    static inline bool SummaryHasPositiveMove(u32 move_id) {
        for (u32 i = 0; i < move_record_count; i++) {
            const MoveScoreRecord& r = move_records[i];
            if (r.move_id == move_id && r.score_sum > 0) return true;
        }
        return false;
    }

    static inline bool SummaryHasSuperEffectivePositiveMove(SpeciesTypeInfo target_types) {
        if (!HasSpeciesTypes(target_types)) return false;
        for (u32 i = 0; i < move_record_count; i++) {
            const MoveScoreRecord& r = move_records[i];
            const u8 mt = MoveType(r.move_id);
            if (r.score_sum > 0 && TypeEffectiveness10(mt, target_types) >= 20) return true;
        }
        return false;
    }

    static inline s32 ScoreSwitchMatchupV18(u64 state_ptr, u16 active_species, u16 switch_species, const char* tag, u32 hit) {
        const SpeciesTypeInfo active_types = SpeciesTypes(active_species);
        const SpeciesTypeInfo switch_types = SpeciesTypes(switch_species);
        const s32 best_move = BestMoveSummaryScore();
        const s32 worst_move = WorstMoveSummaryScore();

        if (!HasSpeciesTypes(switch_types) || !HasSpeciesTypes(active_types)) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s v19_score blocked: unknown_types active=%u/%s switchin=%u/%s best=%d worst=%d\n",
                    tag, static_cast<u32>(active_species), KnownSpeciesName(active_species), static_cast<u32>(switch_species), KnownSpeciesName(switch_species), best_move, worst_move);
            }
            return -9999;
        }

        // Immediate lead utility: Fake Out is a tempo move, but only block switching
        // for the active Fake Out user.  V16 saw the global move summary and could
        // incorrectly block a Gallade switch because Mienshao's Fake Out scored
        // positively elsewhere in the same window.  V17 narrows that rule to
        // Mienshao/active slot only until we map the exact move-owner field.
        if (active_species == 620 && SummaryHasPositiveMove(252)) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s v19_score blocked: active_fakeout_tempo active=%u/%s switchin=%u/%s best=%d worst=%d\n",
                    tag, static_cast<u32>(active_species), KnownSpeciesName(active_species), static_cast<u32>(switch_species), KnownSpeciesName(switch_species), best_move, worst_move);
            }
            return -9999;
        }

        s32 score = 0;
        u32 threat_count = 0;
        u32 known_threat_count = 0;
        u32 worse_defense_count = 0;
        u32 better_defense_count = 0;
        u32 switchin_bad_hits = 0;
        u32 switchin_good_hits = 0;
        u32 active_danger_hits = 0;
        u32 offensive_hits = 0;

        // Move opportunity penalty: if the game likes a move, switching must
        // have a strong matchup reason to override it.
        if (best_move >= 6) score -= 90;
        else if (best_move >= 3) score -= 55;
        else if (best_move >= 1) score -= 25;
        else if (best_move <= -4) score += 30;
        else if (best_move <= 0) score += 8;

        for (u32 i = 0; i < move_record_count; i++) {
            const MoveScoreRecord& r = move_records[i];
            const u16 target_species = r.target_species;
            if (target_species == 0) continue;
            if (IsKnownFriendlyCandidateSpeciesForState(state_ptr, target_species, active_species, switch_species)) continue;
            threat_count++;

            const SpeciesTypeInfo target_types = SpeciesTypes(target_species);
            if (!HasSpeciesTypes(target_types)) continue;
            known_threat_count++;

            const u32 target_stab_vs_active = MaxThreatMoveEffectiveness10(target_species, active_types);
            const u32 target_stab_vs_switch = MaxThreatMoveEffectiveness10(target_species, switch_types);
            const u32 switch_stab_vs_target = MaxSwitchOffenseEffectiveness10(switch_species, target_types);

            // V19: extra caution for Protean/Libero-style targets.  If their plausible move profile
            // threatens the switch-in, do not overvalue the switch just because their current type
            // or base type looks harmless.
            if (SpeciesUsesMoveTypeThreatLogic(target_species) && target_stab_vs_switch >= 20) {
                score -= 40;
                switchin_bad_hits++;
                worse_defense_count++;
            }

            if (target_stab_vs_active >= 20) {
                score += 22;
                active_danger_hits++;
            }
            if (target_stab_vs_switch == 0) {
                score += 35;
                better_defense_count++;
            } else if (target_stab_vs_switch <= 5) {
                score += 22;
                better_defense_count++;
            } else if (target_stab_vs_switch >= 40) {
                score -= 90;
                switchin_bad_hits++;
                worse_defense_count++;
            } else if (target_stab_vs_switch >= 20) {
                score -= 48;
                switchin_bad_hits++;
                worse_defense_count++;
            }

            if (target_stab_vs_switch > target_stab_vs_active) {
                score -= 28;
                worse_defense_count++;
            } else if (target_stab_vs_switch < target_stab_vs_active) {
                score += 18;
                better_defense_count++;
            }

            if (switch_stab_vs_target >= 40) {
                score += 34;
                switchin_good_hits++;
                offensive_hits++;
            } else if (switch_stab_vs_target >= 20) {
                score += 20;
                switchin_good_hits++;
                offensive_hits++;
            } else if (switch_stab_vs_target == 0) {
                score -= 24;
            } else if (switch_stab_vs_target <= 5) {
                score -= 10;
            }

            if (SummaryHasSuperEffectivePositiveMove(target_types)) {
                score -= 25;
            }
        }

        // Role/future-value rules.  These are intentionally broad; HP-based
        // preservation will become more precise once we map active HP for cs_80.
        if (active_species == 620) score -= 18; // Mienshao often has Fake Out / tempo.
        if (switch_species == 620) score += 12; // bringing Fake Out later can be useful.
        if (switch_species == 448) score -= 8;  // Lucario is strong but risky into Fire/Ground/Fighting boards.
        if (switch_species == 534) score += 8;  // Conkeldurr is often a sturdier pivot than Lucario.
        if (switch_species == 675 && MoveSummaryIncludesTargetSpecies(861)) score -= 80; // Pangoro into Fairy board.

        // V17 role heuristics inspired by official switch-in/replacement logic:
        // don't switch into a mon that is only good into one foe if it is highly
        // threatened by the other visible foe; reward pivots that improve defense
        // and still threaten at least one opponent.
        if (switchin_bad_hits >= 1 && switchin_good_hits == 0) score -= 35;
        if (better_defense_count >= 1 && switchin_good_hits >= 1) score += 18;
        if (active_danger_hits >= 1 && best_move <= 0) score += 25;
        if (active_danger_hits >= 2) score += 18;
        if (best_move >= 2 && active_danger_hits == 0) score -= 22;

        if (known_threat_count == 0) score -= 60;
        if (threat_count == 0) score -= 30;
        if (switchin_bad_hits >= 1 && better_defense_count == 0) score -= 70;
        if (active_danger_hits == 0 && best_move > 0) score -= 45;
        if (better_defense_count >= 2) score += 20;
        if (offensive_hits >= 2) score += 12;

        if (ShouldLog(hit)) {
            Logging.Log("[ai_bridge] %s v19_score active=%u/%s(%s/%s) switchin=%u/%s(%s/%s) score=%d best=%d worst=%d threats=%u known=%u bad=%u better=%u active_danger=%u offense=%u\n",
                tag,
                static_cast<u32>(active_species), KnownSpeciesName(active_species), TypeName(active_types.t1), TypeName(active_types.t2),
                static_cast<u32>(switch_species), KnownSpeciesName(switch_species), TypeName(switch_types.t1), TypeName(switch_types.t2),
                score, best_move, worst_move, threat_count, known_threat_count, switchin_bad_hits, better_defense_count, active_danger_hits, offensive_hits);
        }
        return score;
    }

    static inline bool V16MatchupGateAllows(u64 state_ptr, u32 action_id, const char* tag, u32 hit) {
        const u16 switch_in_species = InferredSwitchSpeciesForAction(state_ptr, action_id);
        const u16 active_species = CandidateActiveSpecies(state_ptr);
        LogResolvedTeam(tag, state_ptr, hit);
        const s32 matchup_score = ScoreSwitchMatchupV18(state_ptr, active_species, switch_in_species, tag, hit);
        if (matchup_score < 25) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s v19_gate blocked score=%d threshold=25 active=%u/%s switchin=%u/%s\n",
                    tag, matchup_score, static_cast<u32>(active_species), KnownSpeciesName(active_species), static_cast<u32>(switch_in_species), KnownSpeciesName(switch_in_species));
            }
            return false;
        }
        if (ShouldLog(hit)) {
            Logging.Log("[ai_bridge] %s v19_gate allowed score=%d active=%u/%s switchin=%u/%s\n",
                tag, matchup_score, static_cast<u32>(active_species), KnownSpeciesName(active_species), static_cast<u32>(switch_in_species), KnownSpeciesName(switch_in_species));
        }
        return true;
    }

    static inline bool SpeciesAwareSwitchGateAllows(u64 state_ptr, u32 action_id, const char* tag, u32 hit) {
        if (global_config.ai_bridge.switch_policy_mode == 7) return V16MatchupGateAllows(state_ptr, action_id, tag, hit);
        if (global_config.ai_bridge.switch_policy_mode != 6) return true;

        const u16 switch_in_species = InferredSwitchSpeciesForAction(state_ptr, action_id);
        const u16 active_species = CandidateActiveSpecies(state_ptr);
        const s32 best = BestMoveSummaryScore();
        const s32 worst = WorstMoveSummaryScore();
        const bool sees_cinderace = MoveSummaryIncludesTargetSpecies(815);
        const bool sees_grimmsnarl = MoveSummaryIncludesTargetSpecies(861);

        // Rule 1: do not switch out Mienshao when it has any positive-scoring
        // line, because lead Fake Out / Close Combat pressure is immediate tempo.
        if (active_species == 620 && best > 0) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s species_gate_v15 blocked: fakeout_lead active=%u/%s switchin=%u/%s best=%d worst=%d cinderace=%u grimmsnarl=%u\n",
                    tag,
                    static_cast<u32>(active_species), KnownSpeciesName(active_species),
                    static_cast<u32>(switch_in_species), KnownSpeciesName(switch_in_species),
                    best, worst, sees_cinderace ? 1 : 0, sees_grimmsnarl ? 1 : 0);
            }
            return false;
        }

        // Rule 2: do not send Lucario into a visible Cinderace board unless the
        // active Pokémon is in an actually terrible move-score state.  This is
        // the exact bad switch the user observed: Mienshao -> Lucario into Cinderace.
        if (switch_in_species == 448 && sees_cinderace && best >= 0) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s species_gate_v15 blocked: lucario_into_cinderace active=%u/%s switchin=%u/%s best=%d worst=%d\n",
                    tag,
                    static_cast<u32>(active_species), KnownSpeciesName(active_species),
                    static_cast<u32>(switch_in_species), KnownSpeciesName(switch_in_species),
                    best, worst);
            }
            return false;
        }

        if (ShouldLog(hit)) {
            Logging.Log("[ai_bridge] %s species_gate_v15 allowed active=%u/%s switchin=%u/%s best=%d worst=%d cinderace=%u grimmsnarl=%u\n",
                tag,
                static_cast<u32>(active_species), KnownSpeciesName(active_species),
                static_cast<u32>(switch_in_species), KnownSpeciesName(switch_in_species),
                best, worst, sees_cinderace ? 1 : 0, sees_grimmsnarl ? 1 : 0);
        }
        return true;
    }

    static inline bool MoveSummaryGateAllowsSwitch(const char* tag, u32 hit) {
        if (!global_config.ai_bridge.switch_gate_use_move_summary) return true;

        if (move_record_count < global_config.ai_bridge.switch_gate_min_move_records) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s move_summary_gate blocked: not_enough_records count=%u need=%u\n",
                    tag, move_record_count, global_config.ai_bridge.switch_gate_min_move_records);
            }
            return false;
        }

        const s32 best = BestMoveSummaryScore();
        const s32 worst = WorstMoveSummaryScore();
        bool has_negative = false;
        for (u32 i = 0; i < move_record_count; i++) {
            const MoveScoreRecord& r = move_records[i];
            if (r.score_min <= global_config.ai_bridge.switch_gate_negative_record_threshold ||
                r.score_sum <= global_config.ai_bridge.switch_gate_negative_record_threshold) {
                has_negative = true;
                break;
            }
        }

        if (global_config.ai_bridge.switch_gate_require_negative_record && !has_negative) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s move_summary_gate blocked: no_negative_record best=%d worst=%d count=%u threshold=%d\n",
                    tag, best, worst, move_record_count, global_config.ai_bridge.switch_gate_negative_record_threshold);
            }
            return false;
        }

        if (best > global_config.ai_bridge.switch_gate_best_move_max) {
            if (ShouldLog(hit)) {
                Logging.Log("[ai_bridge] %s move_summary_gate blocked: best_move_too_good best=%d max=%d worst=%d count=%u\n",
                    tag, best, global_config.ai_bridge.switch_gate_best_move_max, worst, move_record_count);
            }
            return false;
        }

        if (ShouldLog(hit)) {
            Logging.Log("[ai_bridge] %s move_summary_gate allowed: best=%d max=%d worst=%d count=%u negative=%u\n",
                tag, best, global_config.ai_bridge.switch_gate_best_move_max, worst, move_record_count, has_negative ? 1 : 0);
        }
        return true;
    }

    static inline void LogMoveEvalFromReadback(u64 out, u32 hit, u32 old_score, u32 old_enable) {
        if (!global_config.ai_bridge.score_survey_mode) return;

        const u32 move_id = MoveIdFromOut(out);
        const u32 target_key = TargetKeyFromOut(out);
        const u64 eval_target = HiLoPtrFromOut(out, 0xA0);
        const u64 move_obj = HiLoPtrFromOut(out, 0xA8);
        const u64 actor_or_side = (static_cast<u64>(*reinterpret_cast<volatile u32*>(out + 0xB8)) << 32)
            | static_cast<u64>(*reinterpret_cast<volatile u32*>(out + 0xB4));
        const s32 score_delta = SignedScore(old_score);

        // V11: In V10, summaries were effectively global and also stopped
        // updating after max_log_hits.  The candidate gate kept seeing an old
        // best=+6 forever.  Now the score survey continues recording even when
        // log output is capped, and it resets the summary after the candidate
        // gate has consumed one complete scoring window.
        const u16 target_species = SpeciesFromEvalTarget(eval_target);

        if (move_records_consumed) {
            ClearMoveScoreRecords();
        }
        RecordMoveScore(move_id, target_key, target_species, score_delta);

        if (!ShouldLog(hit)) return;

        Logging.Log("[ai_bridge] move_eval hit=%u gen=%u move=%u target=%08x target_species=%u/%s score_delta=%d enable=%u eval_target=%016lx move_obj=%016lx actor_or_side=%016lx\n",
            hit, move_summary_generation, move_id, target_key,
            static_cast<u32>(target_species), KnownSpeciesName(target_species),
            score_delta, old_enable, eval_target, move_obj, actor_or_side);

        // V14: target species mapping.  The eval_target object now directly
        // tells us which Pokémon the AI is evaluating against.  This is the
        // first reliable runtime species read and is the basis for matchup-aware
        // switch scoring.
        ScanKnownSpecies("eval_target", eval_target, hit, 0x120);
        ScanKnownSpecies("move_obj", move_obj, hit, 0x180);

        // V13: optional deep survey of the structures that likely identify
        // the actor/side, move object, and target/eval object.  This is the
        // next step toward real bench switch-in scoring: we need to locate
        // active species/party slot/side from these objects instead of only
        // seeing move IDs and candidate IDs.
        if (global_config.ai_bridge.score_survey_dump_state) {
            DumpRawWords("eval_target_raw", eval_target, hit, global_config.ai_bridge.score_survey_words);
            DumpRawWords("move_obj_raw", move_obj, hit, global_config.ai_bridge.score_survey_words);
            DumpRawWords("actor_side_raw", actor_or_side, hit, global_config.ai_bridge.score_survey_words);
        }

        DumpMoveScoreSummary(hit);
    }

    static inline void ForceCandidateEntryIfEnabled(u64 state_ptr, const char* tag, u32 hit) {
        if (!global_config.ai_bridge.force_candidate_entry) return;
        if (hit != 1) return; // legacy one-shot injection, first hit only for safety
        if (state_ptr < 0x100000) return;

        const u32 slot = global_config.ai_bridge.force_candidate_slot;
        if (slot >= 16) return;
        const u64 entry = state_ptr + 0xC8 + (static_cast<u64>(slot) * 8);
        *reinterpret_cast<volatile u8*>(entry + 0x0) = static_cast<u8>(global_config.ai_bridge.force_candidate_action_id & 0xFF);
        *reinterpret_cast<volatile u8*>(entry + 0x1) = global_config.ai_bridge.force_candidate_enable ? 1 : 0;
        *reinterpret_cast<volatile u32*>(entry + 0x4) = global_config.ai_bridge.force_candidate_score;

        // Ensure candidate count reaches this slot.
        volatile u8* count = reinterpret_cast<volatile u8*>(state_ptr + 0xC5);
        if (*count <= slot) *count = static_cast<u8>(slot + 1);

        Logging.Log("[ai_bridge] %s force_candidate slot=%u action=%u enable=%u score=%u\n",
            tag, slot, global_config.ai_bridge.force_candidate_action_id,
            global_config.ai_bridge.force_candidate_enable ? 1 : 0,
            global_config.ai_bridge.force_candidate_score);
    }

    static inline void ResetSwitchPolicyIfNeeded(u64 state_ptr) {
        if (AIBridge::policy_state_ptr == state_ptr) return;
        AIBridge::policy_state_ptr = state_ptr;
        AIBridge::policy_forced_total = 0;
        AIBridge::policy_forced_action2 = 0;
        AIBridge::policy_forced_action3 = 0;
        AIBridge::policy_forced_other = 0;
        AIBridge::policy_cooldown = 0;
        AIBridge::policy_last_preferred_action = 3;
    }

    static inline u32& ForcedCountForAction(u32 action_id) {
        if (action_id == 2) return AIBridge::policy_forced_action2;
        if (action_id == 3) return AIBridge::policy_forced_action3;
        return AIBridge::policy_forced_other;
    }

    static inline u32 TargetScoreForAction(u32 action_id) {
        if (action_id == 2 && global_config.ai_bridge.switch_action2_score != 0) {
            return global_config.ai_bridge.switch_action2_score;
        }
        if (action_id == 3 && global_config.ai_bridge.switch_action3_score != 0) {
            return global_config.ai_bridge.switch_action3_score;
        }
        return global_config.ai_bridge.switch_score;
    }

    static inline bool NativeScoreAllowed(u32 native_score) {
        if (native_score < global_config.ai_bridge.switch_min_native_score) return false;
        if (global_config.ai_bridge.switch_max_native_score != 0 &&
            native_score > global_config.ai_bridge.switch_max_native_score) return false;
        return true;
    }

    static inline u32 CountMatchingSwitchCandidates(u64 state_ptr, u32 count, u32 min_action, u32 max_action) {
        u32 matches = 0;
        for (u32 i = 0; i < count; i++) {
            const u64 entry = state_ptr + 0xC8 + (static_cast<u64>(i) * 8);
            const u32 action_id = static_cast<u32>(*reinterpret_cast<volatile u8*>(entry + 0x0));
            const u32 native_score = *reinterpret_cast<volatile u32*>(entry + 0x4);
            if (action_id < min_action || action_id > max_action) continue;
            if (!NativeScoreAllowed(native_score)) continue;
            matches++;
        }
        return matches;
    }

    static inline bool PreferredActionAllowed(u32 action_id, u32 preferred) {
        if (preferred == 0) return true;
        return action_id == preferred;
    }

    static inline u32 PickPreferredActionForThisHit() {
        const u32 configured = global_config.ai_bridge.switch_preferred_action;
        if (configured == 2 || configured == 3) return configured;
        if (!global_config.ai_bridge.switch_alternate_actions) return 2;
        AIBridge::policy_last_preferred_action = (AIBridge::policy_last_preferred_action == 2) ? 3 : 2;
        return AIBridge::policy_last_preferred_action;
    }

    static inline void CaptureDeferredCandidateState(u64 state_ptr, u32 score_hit) {
        if (global_config.ai_bridge.switch_policy_mode != 5 && global_config.ai_bridge.switch_policy_mode != 6 && global_config.ai_bridge.switch_policy_mode != 7) return;
        if (state_ptr < 0x100000) return;

        volatile u8* b = reinterpret_cast<volatile u8*>(state_ptr);
        const u32 count = ClampCandidateCount(static_cast<u32>(b[0xC5]));
        if (count == 0) return;

        const u32 min_action = global_config.ai_bridge.force_existing_action_min;
        const u32 max_action = global_config.ai_bridge.force_existing_action_max;
        const u32 matching = CountMatchingSwitchCandidates(state_ptr, count, min_action, max_action);
        if (matching == 0) return;

        deferred_candidate_state_ptr = state_ptr;
        deferred_candidate_score_hit = score_hit;
        deferred_candidate_ready = true;
        deferred_policy_applied_this_window = false;

        if (ShouldLog(score_hit)) {
            Logging.Log("[ai_bridge] deferred_capture hit=%u state=%016lx count=%u matching=%u\n",
                score_hit, state_ptr, count, matching);
        }
    }

    static inline void ApplySwitchPolicyIfEnabled(u64 state_ptr, const char* tag, u32 hit) {
        const u32 mode = global_config.ai_bridge.switch_policy_mode;
        if (mode == 0) return;
        if (hit < global_config.ai_bridge.switch_min_hit) return;
        if (state_ptr < 0x100000) return;

        const bool summary_ready_for_gate = global_config.ai_bridge.switch_gate_use_move_summary &&
            move_record_count >= global_config.ai_bridge.switch_gate_min_move_records;
        const bool gate_allows_switch = MoveSummaryGateAllowsSwitch(tag, hit);
        if (summary_ready_for_gate) {
            // Mark this scoring window consumed.  The next readback/move_eval
            // starts a new summary generation.  This prevents a good move from
            // turn 1 from blocking switches forever on later turns.
            move_records_consumed = true;
        }
        if (!gate_allows_switch) return;

        ResetSwitchPolicyIfNeeded(state_ptr);

        if (AIBridge::policy_cooldown != 0) {
            AIBridge::policy_cooldown--;
            return;
        }

        if (global_config.ai_bridge.switch_max_forces_per_battle != 0 &&
            AIBridge::policy_forced_total >= global_config.ai_bridge.switch_max_forces_per_battle) {
            return;
        }

        volatile u8* b = reinterpret_cast<volatile u8*>(state_ptr);
        const u32 count = ClampCandidateCount(static_cast<u32>(b[0xC5]));
        if (count == 0) return;

        const u32 min_action = global_config.ai_bridge.force_existing_action_min;
        const u32 max_action = global_config.ai_bridge.force_existing_action_max;
        const u32 matching = CountMatchingSwitchCandidates(state_ptr, count, min_action, max_action);

        // Conservative default: only act once both active-slot switch candidates
        // are present.  This avoids enabling a lone switch candidate every time
        // the table is partially rebuilt.
        if (global_config.ai_bridge.switch_require_candidate_count != 0 &&
            matching < global_config.ai_bridge.switch_require_candidate_count) {
            return;
        }

        const u32 preferred_action = PickPreferredActionForThisHit();
        const u32 max_changed_this_hit = global_config.ai_bridge.switch_max_candidates_per_hit == 0
            ? 1
            : global_config.ai_bridge.switch_max_candidates_per_hit;
        u32 changed = 0;

        for (u32 pass = 0; pass < 2 && changed < max_changed_this_hit; pass++) {
            for (u32 i = 0; i < count && changed < max_changed_this_hit; i++) {
                const u64 entry = state_ptr + 0xC8 + (static_cast<u64>(i) * 8);
                const u32 action_id = static_cast<u32>(*reinterpret_cast<volatile u8*>(entry + 0x0));
                if (action_id < min_action || action_id > max_action) continue;
                if (pass == 0 && !PreferredActionAllowed(action_id, preferred_action)) continue;
                if (!SpeciesAwareSwitchGateAllows(state_ptr, action_id, tag, hit)) continue;

                volatile u8* enabled = reinterpret_cast<volatile u8*>(entry + 0x1);
                volatile u32* score = reinterpret_cast<volatile u32*>(entry + 0x4);
                const u32 native_score = *score;
                if (!NativeScoreAllowed(native_score)) continue;

                u32& action_count = ForcedCountForAction(action_id);
                if (global_config.ai_bridge.switch_max_forces_per_action != 0 &&
                    action_count >= global_config.ai_bridge.switch_max_forces_per_action) {
                    continue;
                }

                const u32 target_score = TargetScoreForAction(action_id);
                bool did_change = false;

                // mode 1: enable only; keep native score.
                // mode 2: enable and raise to target_score only if lower.
                // mode 3: enable and set target_score every time.
                // mode 4: V7 conservative mode; enable at most one candidate
                //         when the native switch score is in the allowed range.
                if (!*enabled) {
                    *enabled = 1;
                    did_change = true;
                }

                if (!global_config.ai_bridge.switch_native_score_only) {
                    if ((mode == 2 || mode == 4 || mode == 5 || mode == 7) && *score < target_score) {
                        *score = target_score;
                        did_change = true;
                    } else if (mode >= 3 && mode != 4 && mode != 5 && *score != target_score) {
                        *score = target_score;
                        did_change = true;
                    }
                }

                if (did_change) {
                    changed++;
                    AIBridge::policy_forced_total++;
                    action_count++;
                    if (global_config.ai_bridge.switch_disable_after_force) {
                        for (u32 j = i + 1; j < count; j++) {
                            const u64 other = state_ptr + 0xC8 + (static_cast<u64>(j) * 8);
                            const u32 other_action = static_cast<u32>(*reinterpret_cast<volatile u8*>(other + 0x0));
                            if (other_action >= min_action && other_action <= max_action) {
                                *reinterpret_cast<volatile u8*>(other + 0x1) = 0;
                            }
                        }
                    }
                    if (global_config.ai_bridge.switch_cooldown_hits != 0) {
                        AIBridge::policy_cooldown = global_config.ai_bridge.switch_cooldown_hits;
                    }
                }
            }
        }

        if (changed != 0 && ShouldLog(hit)) {
            Logging.Log("[ai_bridge] %s switch_policy_v19 mode=%u changed=%u total=%u a2=%u a3=%u other=%u cooldown=%u score=%u matching=%u preferred=%u native_only=%u\n",
                tag, mode, changed,
                AIBridge::policy_forced_total,
                AIBridge::policy_forced_action2,
                AIBridge::policy_forced_action3,
                AIBridge::policy_forced_other,
                AIBridge::policy_cooldown,
                global_config.ai_bridge.switch_score,
                matching,
                preferred_action,
                global_config.ai_bridge.switch_native_score_only ? 1 : 0);
            DumpCandidateTable("candidate_score_after_switch_policy_v19", state_ptr, hit);
        }
    }

    static inline void ApplyDeferredSwitchPolicyIfReady(u32 output_hit) {
        if (global_config.ai_bridge.switch_policy_mode != 5 && global_config.ai_bridge.switch_policy_mode != 6 && global_config.ai_bridge.switch_policy_mode != 7) return;
        if (!deferred_candidate_ready || deferred_policy_applied_this_window) return;
        if (deferred_candidate_state_ptr < 0x100000) return;

        // Wait until the current move scoring window has enough records.
        // Existing config keys control the threshold: switch_gate_min_move_records
        // and switch_min_hit.  In V12, switch_min_hit is interpreted as an
        // output/readback hit threshold for the deferred application.
        if (output_hit < global_config.ai_bridge.switch_min_hit) return;
        if (global_config.ai_bridge.switch_gate_use_move_summary &&
            move_record_count < global_config.ai_bridge.switch_gate_min_move_records) {
            if (ShouldLog(output_hit)) {
                Logging.Log("[ai_bridge] deferred_switch blocked: not_enough_records output_hit=%u records=%u need=%u\n",
                    output_hit, move_record_count, global_config.ai_bridge.switch_gate_min_move_records);
            }
            return;
        }

        if (ShouldLog(output_hit)) {
            Logging.Log("[ai_bridge] deferred_switch attempting output_hit=%u candidate_hit=%u state=%016lx records=%u best=%d worst=%d\n",
                output_hit, deferred_candidate_score_hit, deferred_candidate_state_ptr,
                move_record_count, BestMoveSummaryScore(), WorstMoveSummaryScore());
        }

        ApplySwitchPolicyIfEnabled(deferred_candidate_state_ptr, "deferred_candidate_score", output_hit);
        deferred_policy_applied_this_window = true;
    }

    static inline void ForceExistingCandidatesIfEnabled(u64 state_ptr, const char* tag, u32 hit) {
        if (!global_config.ai_bridge.force_existing_candidates) return;
        if (state_ptr < 0x100000) return;

        volatile u8* b = reinterpret_cast<volatile u8*>(state_ptr);
        const u32 count = ClampCandidateCount(static_cast<u32>(b[0xC5]));
        if (count == 0) return;

        const u32 min_action = global_config.ai_bridge.force_existing_action_min;
        const u32 max_action = global_config.ai_bridge.force_existing_action_max;
        u32 changed = 0;

        for (u32 i = 0; i < count; i++) {
            const u64 entry = state_ptr + 0xC8 + (static_cast<u64>(i) * 8);
            const u32 action_id = static_cast<u32>(*reinterpret_cast<volatile u8*>(entry + 0x0));
            if (action_id < min_action || action_id > max_action) continue;

            *reinterpret_cast<volatile u8*>(entry + 0x1) = global_config.ai_bridge.force_existing_enable ? 1 : 0;
            *reinterpret_cast<volatile u32*>(entry + 0x4) = global_config.ai_bridge.force_existing_score;
            changed++;
        }

        if (changed != 0 && ShouldLog(hit)) {
            Logging.Log("[ai_bridge] %s force_existing changed=%u count=%u action_range=%u-%u enable=%u score=%u\n",
                tag, changed, count, min_action, max_action,
                global_config.ai_bridge.force_existing_enable ? 1 : 0,
                global_config.ai_bridge.force_existing_score);
            DumpCandidateTable("candidate_score_after_force_existing", state_ptr, hit);
        }
    }

    static inline void ForceFinalSelectionIfEnabled(u64 state_ptr, const char* tag, u32 hit) {
        if (!global_config.ai_bridge.force_final_selection) return;
        if (state_ptr < 0x100000) return;

        *reinterpret_cast<volatile u8*>(state_ptr + 0xF8) = 1;
        *reinterpret_cast<volatile u32*>(state_ptr + 0xFC) = global_config.ai_bridge.force_final_score;
        *reinterpret_cast<volatile u32*>(state_ptr + 0x100) = global_config.ai_bridge.force_final_action_id;

        if (ShouldLog(hit)) {
            Logging.Log("[ai_bridge] %s force_final action=%u score=%u\n",
                tag, global_config.ai_bridge.force_final_action_id,
                global_config.ai_bridge.force_final_score);
        }
    }

    // V12 one-shot action mapper.  This is for identifying what action=2 and
    // action=3 actually mean in live battle: which active slot switches out and
    // what replacement logic chooses.  It deliberately forces only a limited
    // number of matching candidates, unlike the old v5 force-existing proof.
    inline u32 action_probe_uses = 0;

    static inline void ApplyActionProbeIfEnabled(u64 state_ptr, const char* tag, u32 hit) {
        if (!global_config.ai_bridge.action_probe_enabled) return;
        if (action_probe_uses >= global_config.ai_bridge.action_probe_max_uses) return;
        if (state_ptr < 0x100000) return;

        volatile u8* b = reinterpret_cast<volatile u8*>(state_ptr);
        const u32 count = ClampCandidateCount(static_cast<u32>(b[0xC5]));
        const u32 wanted = global_config.ai_bridge.action_probe_action_id;
        for (u32 i = 0; i < count && i < 16; i++) {
            const u64 entry = state_ptr + 0xC8 + (static_cast<u64>(i) * 8);
            volatile u8* eb = reinterpret_cast<volatile u8*>(entry);
            volatile u32* score_ptr = reinterpret_cast<volatile u32*>(entry + 0x4);
            const u32 action_id = static_cast<u32>(eb[0]);
            if (action_id != wanted) continue;

            const u32 old_enabled = static_cast<u32>(eb[1]);
            const u32 old_score = *score_ptr;
            eb[1] = 1;
            *score_ptr = global_config.ai_bridge.action_probe_score;
            action_probe_uses++;
            Logging.Log("[ai_bridge] action_probe %s hit=%u use=%u/%u slot=%u action=%u old_enabled=%u old_score=%u new_score=%u\n",
                tag, hit, action_probe_uses, global_config.ai_bridge.action_probe_max_uses,
                i, action_id, old_enabled, old_score, global_config.ai_bridge.action_probe_score);
            return;
        }
    }

}

HOOK_DEFINE_INLINE(AIBridgeOutputReadbackPostProbe) {
    static void Callback(exl::hook::nx64::InlineCtx* ctx) {
        if (!global_config.initialized || !global_config.ai_bridge.active) return;
        AIBridge::output_hits++;

        // In this function x19 is the output object x1 passed to 0x7D4B80.
        const u64 out = ctx->X[19];
        if (out >= 0x100000) {
            volatile u32* score_ptr = reinterpret_cast<volatile u32*>(out + 0x0);
            volatile u8* enable_ptr = reinterpret_cast<volatile u8*>(out + 0x4);
            const u32 old_score = *score_ptr;
            const u32 old_enable = *enable_ptr;

            if (AIBridge::ShouldLog(AIBridge::output_hits)) {
                Logging.Log("[ai_bridge] readback_post hit=%u out=%016lx score=%u enable=%u x0=%016lx x19=%016lx x30=%016lx\n",
                    AIBridge::output_hits, out, old_score, old_enable, ctx->X[0], ctx->X[19], ctx->X[30]);
            }

            AIBridge::LogMoveEvalFromReadback(out, AIBridge::output_hits, old_score, old_enable);
            AIBridge::ApplyDeferredSwitchPolicyIfReady(AIBridge::output_hits);

            if (global_config.ai_bridge.score_survey_mode) {
                if (global_config.ai_bridge.score_survey_dump_readback_out) {
                    AIBridge::DumpRawWords("readback_out_raw", out, AIBridge::output_hits, global_config.ai_bridge.score_survey_words);
                }
                if (global_config.ai_bridge.score_survey_dump_readback_x0) {
                    AIBridge::DumpRawWords("readback_x0_raw", ctx->X[0], AIBridge::output_hits, global_config.ai_bridge.score_survey_words);
                }
            }

            if (global_config.ai_bridge.force_output_score != 0) {
                *score_ptr = global_config.ai_bridge.force_output_score;
            }
            if (global_config.ai_bridge.force_pokechange_enable >= 0) {
                *enable_ptr = static_cast<u8>(global_config.ai_bridge.force_pokechange_enable & 1);
            }
        }
    }
};

HOOK_DEFINE_INLINE(AIBridgeCandidateListPostProbe) {
    static void Callback(exl::hook::nx64::InlineCtx* ctx) {
        if (!global_config.initialized || !global_config.ai_bridge.active) return;
        AIBridge::list_hits++;
        // x19 is the candidate-selection state object in this function.
        AIBridge::DumpCandidateTable("candidate_list", ctx->X[19], AIBridge::list_hits);
        AIBridge::ForceCandidateEntryIfEnabled(ctx->X[19], "candidate_list", AIBridge::list_hits);
    }
};

HOOK_DEFINE_INLINE(AIBridgeCandidateScorePostProbe) {
    static void Callback(exl::hook::nx64::InlineCtx* ctx) {
        if (!global_config.initialized || !global_config.ai_bridge.active) return;
        AIBridge::score_hits++;
        AIBridge::DumpCandidateTable("candidate_score", ctx->X[19], AIBridge::score_hits);
        if (global_config.ai_bridge.score_survey_dump_state) {
            AIBridge::DumpRawWords("candidate_state_raw", ctx->X[19], AIBridge::score_hits, global_config.ai_bridge.score_survey_words);
            AIBridge::DumpCandidatePointerRefs(ctx->X[19], AIBridge::score_hits);
        }
        // V12 one-shot action mapper.  Prefer this over the old force-existing proof
        // when trying to identify action IDs.
        AIBridge::ApplyActionProbeIfEnabled(ctx->X[19], "candidate_score", AIBridge::score_hits);

        // V5: if the existing candidate rows are switch candidates, this turns
        // them on and gives them a dominant score every time they appear.
        AIBridge::ForceExistingCandidatesIfEnabled(ctx->X[19], "candidate_score", AIBridge::score_hits);
        if (global_config.ai_bridge.switch_policy_mode == 5 || global_config.ai_bridge.switch_policy_mode == 6 || global_config.ai_bridge.switch_policy_mode == 7) {
            AIBridge::CaptureDeferredCandidateState(ctx->X[19], AIBridge::score_hits);
        } else {
            AIBridge::ApplySwitchPolicyIfEnabled(ctx->X[19], "candidate_score", AIBridge::score_hits);
        }
        AIBridge::ForceCandidateEntryIfEnabled(ctx->X[19], "candidate_score", AIBridge::score_hits);
    }
};

HOOK_DEFINE_INLINE(AIBridgeFinalSelectionPostProbe) {
    static void Callback(exl::hook::nx64::InlineCtx* ctx) {
        if (!global_config.initialized || !global_config.ai_bridge.active) return;
        AIBridge::final_hits++;
        AIBridge::DumpCandidateTable("final_selection", ctx->X[19], AIBridge::final_hits);
        AIBridge::ForceFinalSelectionIfEnabled(ctx->X[19], "final_selection", AIBridge::final_hits);
        // Log again after the force so we can confirm memory changed.
        if (global_config.ai_bridge.force_final_selection) {
            AIBridge::DumpCandidateTable("final_selection_after_force", ctx->X[19], AIBridge::final_hits);
        }
    }
};

void install_ai_bridge_patch() {
    if (AIBridge::installed) return;
    if (!global_config.initialized) return;
    if (!global_config.ai_bridge.active) return;

    if (!global_config.ai_bridge.install_candidate_hooks) {
        Logging.Log("[ai_bridge] active but install_candidate_hooks=false; no hooks installed\n");
        AIBridge::installed = true;
        return;
    }

#ifdef VERSION_SHIELD
    Logging.Log("[ai_bridge] Shield offsets are not mapped. No AI bridge hooks installed.\n");
#else
    if (global_config.ai_bridge.hook_output_readback_post) {
        AIBridgeOutputReadbackPostProbe::InstallAtOffset(global_config.ai_bridge.output_readback_post_offset);
        Logging.Log("[ai_bridge] installed readback_post at 0x%lx\n", global_config.ai_bridge.output_readback_post_offset);
    }
    if (global_config.ai_bridge.hook_candidate_list_post) {
        AIBridgeCandidateListPostProbe::InstallAtOffset(global_config.ai_bridge.candidate_list_post_offset);
        Logging.Log("[ai_bridge] installed candidate_list_post at 0x%lx\n", global_config.ai_bridge.candidate_list_post_offset);
    }
    if (global_config.ai_bridge.hook_candidate_score_post) {
        AIBridgeCandidateScorePostProbe::InstallAtOffset(global_config.ai_bridge.candidate_score_post_offset);
        Logging.Log("[ai_bridge] installed candidate_score_post at 0x%lx\n", global_config.ai_bridge.candidate_score_post_offset);
    }
    if (global_config.ai_bridge.hook_final_selection_post) {
        AIBridgeFinalSelectionPostProbe::InstallAtOffset(global_config.ai_bridge.final_selection_post_offset);
        Logging.Log("[ai_bridge] installed final_selection_post at 0x%lx\n", global_config.ai_bridge.final_selection_post_offset);
    }
#endif

    AIBridge::installed = true;
}
