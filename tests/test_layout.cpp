// every expected address & word below is derived by hand, so a mismatch points at the code
#include "check.h"

#include <filesystem>
#include <string>
#include <vector>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/layout.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/relink.h"
#include "Decomp2Gecko/util.h"
#include "Decomp2Gecko/verify.h"
#include "synthetic.h"

using namespace Decomp2Gecko;
using synthetic::ElfBuilder;
using synthetic::words;

namespace {
//
// .text  80003100  main_loop (0x20) helper (0x10) unused_fn (0x10)   melee/a.c
//        80003140  grown_fn (0x10) orphan_fn (0x10) inline_fn (0x10) melee/b.c
//        80190B00  tourney_fn (0x10)                                  melee/gm/tourney.c, reserved range
// .data  80400000  table (0x8: &helper, &main_loop) constant_blob (0x8)
// .sdata 804DB000  sd_counter (4) sd_flag (4)      r13 = 804DB6A0, so sd_counter is at r13-0x6A0
// .bss   80500000  buffer (0x10)

constexpr int64_t CODE_FLAGS = SHF_ALLOC | SHF_EXECINSTR;
constexpr int64_t DATA_FLAGS = SHF_ALLOC | SHF_WRITE;

const Bytes HELPER_BODY = words({0x38600001, 0x4E800020, 0x60000000, 0x60000000}); // li r3,1 / blr
const Bytes UNUSED_BODY = words({0x7C0802A6, 0x48000001, 0x7C0803A6, 0x4E800020}); // mflr / bl inline_fn / mtlr / blr
const Bytes INLINE_BODY = words({0x38600009, 0x4E800020, 0x60000000, 0x60000000}); // li r3,9 / blr
const Bytes ORPHAN_BODY = words({0x38600003, 0x4E800020, 0x60000000, 0x60000000}); // li r3,3 / blr
const Bytes TOURNEY_BODY = words({0x38600004, 0x4E800020, 0x60000000, 0x60000000}); // li r3,4 / blr
const Bytes BLOB = words({0xDEADBEEF, 0xCAFEF00D});
const Bytes SMALL_DATA = words({0x00000005, 0x00000001});

// main_loop as compiled: mflr / stw / bl <callee> / lwz r0,sd_counter(r13) / lis+addi r3,table / <tail> / blr
Bytes main_loop_body(uint32_t tail) {
    return words({0x7C0802A6, 0x90010004, 0x48000001, 0x800D0000, 0x3C600000, 0x38630000, tail, 0x4E800020});
}

const std::string SYMBOLS = "main_loop = .text:0x80003100; // type:function size:0x20 scope:global\n"
                            "helper = .text:0x80003120; // type:function size:0x10 scope:global\n"
                            "unused_fn = .text:0x80003130; // type:function size:0x10 scope:global\n"
                            "grown_fn = .text:0x80003140; // type:function size:0x10 scope:global\n"
                            "orphan_fn = .text:0x80003150; // type:function size:0x10 scope:global\n"
                            "inline_fn = .text:0x80003160; // type:function size:0x10 scope:weak\n"
                            "tourney_fn = .text:0x80190B00; // type:function size:0x10 scope:global\n"
                            "table = .data:0x80400000; // type:object size:0x8 scope:global\n"
                            "constant_blob = .data:0x80400008; // type:object size:0x8 scope:global\n"
                            "sd_counter = .sdata:0x804DB000; // type:object size:0x4 scope:global\n"
                            "sd_flag = .sdata:0x804DB004; // type:object size:0x4 scope:global\n"
                            "buffer = .bss:0x80500000; // type:object size:0x10 scope:global\n";

const std::string SPLITS = "melee/a.c:\n"
                           "\t.text       start:0x80003100 end:0x80003140\n"
                           "\t.data       start:0x80400000 end:0x80400010\n"
                           "\t.sdata      start:0x804DB000 end:0x804DB008\n"
                           "\t.bss        start:0x80500000 end:0x80500010\n\n"
                           "melee/b.c:\n"
                           "\t.text       start:0x80003140 end:0x80003170\n\n"
                           "melee/gm/tourney.c:\n"
                           "\t.text       start:0x80190B00 end:0x80190B10\n";

const std::vector<std::string> UNITS = {"melee/a.c", "melee/b.c", "melee/gm/tourney.c"};

Bytes vanilla_dol() {
    // linked vanilla bytes, every relocation field filled in by hand
    //   main_loop  bl helper      80003108 -> 80003120: +0x18       48000019
    //              lwz sd_counter r13 - 0x6A0 = 804DB000            800DF960
    //              lis/addi table 80400000: ha 8040, lo 0000        3C608040 38630000
    //   unused_fn  bl inline_fn   80003134 -> 80003160: +0x2C       4800002D
    //   grown_fn   bl helper      80003144 -> 80003120: -0x24       4BFFFFDD
    Bytes text =
        words({0x7C0802A6, 0x90010004, 0x48000019, 0x800DF960, 0x3C608040, 0x38630000, 0x80010004, 0x4E800020}) +
        HELPER_BODY + words({0x7C0802A6, 0x4800002D, 0x7C0803A6, 0x4E800020}) +
        words({0x7C0802A6, 0x4BFFFFDD, 0x7C0803A6, 0x4E800020}) + ORPHAN_BODY + INLINE_BODY;
    Bytes data = words({0x80003120, 0x80003100}) + BLOB;
    return synthetic::build_dol(
        {{0x80003100, text}, {0x80190B00, TOURNEY_BODY}}, {{0x80400000, data}, {0x804DB000, SMALL_DATA}});
}

// a.c's inline_fn is the out-of-line copy of an inline that b.c also defines
ElfBuilder clean_a() {
    ElfBuilder object;
    object.section(".text", CODE_FLAGS, main_loop_body(0x80010004) + HELPER_BODY + UNUSED_BODY + INLINE_BODY)
        .section(".data", DATA_FLAGS, words({0, 0}) + BLOB, 8)
        .section(".sdata", DATA_FLAGS, SMALL_DATA, 8)
        .bss(".bss", 0x10)
        .symbol("main_loop", ".text", 0x00, 0x20, STT_FUNC, STB_GLOBAL)
        .symbol("helper", ".text", 0x20, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("unused_fn", ".text", 0x30, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("inline_fn", ".text", 0x40, 0x10, STT_FUNC, STB_WEAK)
        .symbol("table", ".data", 0x00, 0x08, STT_OBJECT, STB_GLOBAL)
        .symbol("constant_blob", ".data", 0x08, 0x08, STT_OBJECT, STB_GLOBAL)
        .symbol("sd_counter", ".sdata", 0x00, 0x04, STT_OBJECT, STB_GLOBAL)
        .symbol("sd_flag", ".sdata", 0x04, 0x04, STT_OBJECT, STB_GLOBAL)
        .symbol("buffer", ".bss", 0x00, 0x10, STT_OBJECT, STB_GLOBAL)
        .reloc(".text", 0x08, R_PPC_REL24, "helper")
        .reloc(".text", 0x0E, R_PPC_EMB_SDA21, "sd_counter")
        .reloc(".text", 0x12, R_PPC_ADDR16_HA, "table")
        .reloc(".text", 0x16, R_PPC_ADDR16_LO, "table")
        .reloc(".text", 0x34, R_PPC_REL24, "inline_fn")
        .reloc(".data", 0x00, R_PPC_ADDR32, "helper")
        .reloc(".data", 0x04, R_PPC_ADDR32, "main_loop");
    return object;
}

ElfBuilder clean_b() {
    ElfBuilder object;
    object
        .section(
            ".text", CODE_FLAGS, words({0x7C0802A6, 0x48000001, 0x7C0803A6, 0x4E800020}) + ORPHAN_BODY + INLINE_BODY)
        .symbol("grown_fn", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("orphan_fn", ".text", 0x10, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("inline_fn", ".text", 0x20, 0x10, STT_FUNC, STB_WEAK)
        .undefined("helper")
        .reloc(".text", 0x04, R_PPC_REL24, "helper");
    return object;
}

ElfBuilder tourney(bool changed) {
    ElfBuilder object;
    object
        .section(".text", CODE_FLAGS, changed ? words({0x38600044, 0x4E800020, 0x60000000, 0x60000000}) : TOURNEY_BODY)
        .symbol("tourney_fn", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL);
    return object;
}

// what the mod changes in melee/a.c & melee/b.c, defaults = the "good" mod
struct ModOptions {
    bool grow_small_data = false; // sd_counter -> 8 bytes
    bool new_small_data = false; // new .sdata object appears
    int64_t new_buf_size = 0x20; // new bss object new_fn points at
};

// the mod: main_loop now calls grown_fn & ends differently, table points at the new function,
// new_fn references a new bss object, nothing references dead_fn
ElfBuilder mod_a(const ModOptions& options = {}) {
    // lis/addi r4,new_buf / blr; w/ new small data also lwz sd_new, so sd_new is referenced
    Bytes new_fn = words({0x3C800000, 0x38840000, 0x4E800020, 0x60000000});
    if (options.new_small_data) {
        new_fn = words({0x3C800000, 0x38840000, 0x80AD0000, 0x4E800020});
    }
    Bytes dead_fn = words({0x38600063, 0x4E800020, 0x60000000, 0x60000000}); // li r3,99 / blr
    Bytes small_data = options.grow_small_data ? words({5, 0, 1}) : SMALL_DATA;
    int64_t sd_flag_offset = options.grow_small_data ? 8 : 4;
    if (options.new_small_data) {
        small_data += words({7});
    }

    ElfBuilder object;
    object
        .section(".text", CODE_FLAGS,
            main_loop_body(0x38000007) + HELPER_BODY + UNUSED_BODY + INLINE_BODY + new_fn + dead_fn)
        .section(".data", DATA_FLAGS, words({0, 0}) + BLOB, 8)
        .section(".sdata", DATA_FLAGS, small_data, 8)
        .bss(".bss", 0x10 + options.new_buf_size)
        .symbol("main_loop", ".text", 0x00, 0x20, STT_FUNC, STB_GLOBAL)
        .symbol("helper", ".text", 0x20, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("unused_fn", ".text", 0x30, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("inline_fn", ".text", 0x40, 0x10, STT_FUNC, STB_WEAK)
        .symbol("new_fn", ".text", 0x50, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("dead_fn", ".text", 0x60, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x00, 0x08, STT_OBJECT, STB_GLOBAL)
        .symbol("constant_blob", ".data", 0x08, 0x08, STT_OBJECT, STB_GLOBAL)
        .symbol("sd_counter", ".sdata", 0x00, options.grow_small_data ? 8 : 4, STT_OBJECT, STB_GLOBAL)
        .symbol("sd_flag", ".sdata", sd_flag_offset, 0x04, STT_OBJECT, STB_GLOBAL)
        .symbol("buffer", ".bss", 0x00, 0x10, STT_OBJECT, STB_GLOBAL)
        .symbol("new_buf", ".bss", 0x10, options.new_buf_size, STT_OBJECT, STB_GLOBAL)
        .undefined("grown_fn")
        .reloc(".text", 0x08, R_PPC_REL24, "grown_fn")
        .reloc(".text", 0x0E, R_PPC_EMB_SDA21, "sd_counter")
        .reloc(".text", 0x12, R_PPC_ADDR16_HA, "table")
        .reloc(".text", 0x16, R_PPC_ADDR16_LO, "table")
        .reloc(".text", 0x34, R_PPC_REL24, "inline_fn")
        .reloc(".text", 0x52, R_PPC_ADDR16_HA, "new_buf")
        .reloc(".text", 0x56, R_PPC_ADDR16_LO, "new_buf")
        .reloc(".data", 0x00, R_PPC_ADDR32, "helper")
        .reloc(".data", 0x04, R_PPC_ADDR32, "new_fn");

    if (options.new_small_data) {
        object.symbol("sd_new", ".sdata", sd_flag_offset + 4, 4, STT_OBJECT, STB_GLOBAL)
            .reloc(".text", 0x5A, R_PPC_EMB_SDA21, "sd_new");
    }
    return object;
}

// grown_fn doubled & calls helper_copy (helper's bytes); orphan_fn grew but lost its references
const Bytes GROWN_BODY =
    words({0x7C0802A6, 0x48000001, 0x48000001, 0x38600005, 0x38600006, 0x7C0803A6, 0x4E800020, 0x60000000});

ElfBuilder mod_b() {
    Bytes orphan =
        words({0x38600003, 0x38600003, 0x38600003, 0x38600003, 0x4E800020, 0x60000000, 0x60000000, 0x60000000});
    ElfBuilder object;
    object.section(".text", CODE_FLAGS, GROWN_BODY + orphan + INLINE_BODY + HELPER_BODY)
        .symbol("grown_fn", ".text", 0x00, 0x20, STT_FUNC, STB_GLOBAL)
        .symbol("orphan_fn", ".text", 0x20, 0x20, STT_FUNC, STB_GLOBAL)
        .symbol("inline_fn", ".text", 0x40, 0x10, STT_FUNC, STB_WEAK)
        .symbol("helper_copy", ".text", 0x50, 0x10, STT_FUNC, STB_GLOBAL)
        .undefined("helper")
        .reloc(".text", 0x04, R_PPC_REL24, "helper")
        .reloc(".text", 0x08, R_PPC_REL24, "helper_copy");
    return object;
}

// the mod as MWLD links it. .sdata moved, so every SDA21 field decodes w/ the linked r13
//   .text 80003100: main_loop helper unused_fn inline_fn new_fn dead_fn | grown_fn orphan_fn helper_copy
//                   +00       +20    +30       +40       +50    +60     | +70      +90       +B0
//   .data 80400000, .sdata 804DB100 (r13 = 804DB7A0), .bss 80500000: buffer new_buf(+10)
Bytes linked_main_elf(bool corrupt_grown_fn_call) {
    //   main_loop  bl grown_fn   80003108 -> 80003170: +0x68        48000069
    //   unused_fn  bl inline_fn  80003134 -> 80003140: +0x0C        4800000D
    //   new_fn     lis/addi new_buf 80500010: ha 8050, lo 0010      3C808050 38840010
    //   grown_fn   bl helper     80003174 -> 80003120: -0x54        4BFFFFAD
    //              bl helper_copy 80003178 -> 800031B0: +0x38       48000039
    uint32_t call_helper = corrupt_grown_fn_call ? 0x4BFFFFB1 : 0x4BFFFFAD;
    Bytes text =
        words({0x7C0802A6, 0x90010004, 0x48000069, 0x800DF960, 0x3C608040, 0x38630000, 0x38000007, 0x4E800020}) +
        HELPER_BODY + words({0x7C0802A6, 0x4800000D, 0x7C0803A6, 0x4E800020}) + INLINE_BODY +
        words({0x3C808050, 0x38840010, 0x4E800020, 0x60000000}) +
        words({0x38600063, 0x4E800020, 0x60000000, 0x60000000}) +
        words({0x7C0802A6, call_helper, 0x48000039, 0x38600005, 0x38600006, 0x7C0803A6, 0x4E800020, 0x60000000}) +
        words({0x38600003, 0x38600003, 0x38600003, 0x38600003, 0x4E800020, 0x60000000, 0x60000000, 0x60000000}) +
        HELPER_BODY;

    ElfBuilder elf;
    elf.section(".text", CODE_FLAGS, text, 4, 0x80003100)
        .section(".text2", CODE_FLAGS, TOURNEY_BODY, 4, 0x80190B00)
        .section(".data", DATA_FLAGS, words({0x80003120, 0x80003150}) + BLOB, 8, 0x80400000)
        .section(".sdata", DATA_FLAGS, SMALL_DATA, 8, 0x804DB100)
        .bss(".bss", 0x30, 8, 0x80500000)
        .symbol("main_loop", ".text", 0x80003100, 0x20, STT_FUNC, STB_GLOBAL)
        .symbol("helper", ".text", 0x80003120, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("unused_fn", ".text", 0x80003130, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("inline_fn", ".text", 0x80003140, 0x10, STT_FUNC, STB_WEAK)
        .symbol("new_fn", ".text", 0x80003150, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("dead_fn", ".text", 0x80003160, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("grown_fn", ".text", 0x80003170, 0x20, STT_FUNC, STB_GLOBAL)
        .symbol("orphan_fn", ".text", 0x80003190, 0x20, STT_FUNC, STB_GLOBAL)
        .symbol("helper_copy", ".text", 0x800031B0, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("tourney_fn", ".text2", 0x80190B00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x80400000, 0x08, STT_OBJECT, STB_GLOBAL)
        .symbol("constant_blob", ".data", 0x80400008, 0x08, STT_OBJECT, STB_GLOBAL)
        .symbol("sd_counter", ".sdata", 0x804DB100, 0x04, STT_OBJECT, STB_GLOBAL)
        .symbol("sd_flag", ".sdata", 0x804DB104, 0x04, STT_OBJECT, STB_GLOBAL)
        .symbol("buffer", ".bss", 0x80500000, 0x10, STT_OBJECT, STB_GLOBAL)
        .symbol("new_buf", ".bss", 0x80500010, 0x20, STT_OBJECT, STB_GLOBAL)
        .absolute("_SDA_BASE_", 0x804DB7A0)
        .absolute("_SDA2_BASE_", 0x804DF9E0);
    return elf.build(true);
}

synthetic::Checkout write_checkout(
    const synthetic::ScratchDir& scratch, const ElfBuilder& a, const ElfBuilder& b, const ElfBuilder& tourney_unit) {
    synthetic::Checkout checkout(scratch.path / "melee");
    checkout.vanilla(vanilla_dol(), SYMBOLS, SPLITS);
    checkout.config(UNITS);
    checkout.object("melee/a", a.build());
    checkout.object("melee/b", b.build());
    checkout.object("melee/gm/tourney", tourney_unit.build());
    return checkout;
}

synthetic::Checkout clean_checkout(const synthetic::ScratchDir& scratch) {
    return write_checkout(scratch, clean_a(), clean_b(), tourney(false));
}

synthetic::Checkout mod_checkout(const synthetic::ScratchDir& scratch, const ModOptions& options = {}) {
    return write_checkout(scratch, mod_a(options), mod_b(), tourney(false));
}

const Chunk* chunk_named(const Layout& layout, const std::string& unit, const std::string& name) {
    for (const Chunk* chunk : layout.chunks()) {
        if (chunk->unit->name == unit && chunk->name() == name) {
            return chunk;
        }
    }
    throw testing::Failure("no chunk " + unit + ":" + name);
}

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

bool any_line_contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& line : lines) {
        if (contains(line, needle)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(layout_clean_tree) {
    synthetic::ScratchDir scratch;
    Layout layout(clean_checkout(scratch).root);

    // 13 chunks: 9 in a.c, 3 in b.c, 1 in tourney.c. b.c's inline_fn is a dropped duplicate
    CHECK_EQ(layout.report_lines()[0],
        std::string(
            "units: 3  chunks: 13  unchanged: 12  in-place: 0  patched: 0  relocated: 0  new: 0  aliased: 0  dropped: 1"));
    CHECK(layout.errors().empty());
    CHECK_EQ(layout.free_used(), int64_t(0));
    CHECK(chunk_named(layout, "melee/b.c", "inline_fn")->action == ChunkAction::Duplicate);
    CHECK(chunk_named(layout, "melee/a.c", "inline_fn")->action == ChunkAction::Same);

    GenerateOptions options;
    options.name = "Clean";
    options.slippi_ini = SlippiIniSource::none();
    GenerateResult generated = generate(layout, options);

    CHECK(generated.status == GenerateResult::Status::Ok);
    CHECK_EQ(generated.codes.gecko_lines, size_t(0));
    CHECK_EQ(generated.ini_text, std::string("[Gecko]\n$Clean [Decomp2Gecko]\n\n[Gecko_Enabled]\n$Clean\n"));
}

TEST(layout_placement) {
    synthetic::ScratchDir scratch;
    std::vector<std::string> log;
    Layout layout(mod_checkout(scratch).root, [&log](const std::string& line) { log.push_back(line); });

    // grown_fn doubled, but orphan_fn right behind it is dropped, so it grows into that slot
    CHECK_EQ(layout.report_lines()[0],
        std::string(
            "units: 3  chunks: 17  unchanged: 8  in-place: 3  patched: 0  relocated: 0  new: 2  aliased: 1  dropped: 3"));
    CHECK_EQ(layout.report_lines()[1], std::string("free region: 0x81700000 used 0x30 of 0x100000"));
    CHECK(layout.errors().empty());
    CHECK_EQ(log.size(), size_t(1));
    CHECK(contains(log[0], "loaded 3 units, 17 chunks"));

    const Chunk* main_loop = chunk_named(layout, "melee/a.c", "main_loop");
    CHECK(main_loop->action == ChunkAction::InPlace);
    CHECK_EQ(*main_loop->placed_address, int64_t(0x80003100));
    CHECK(chunk_named(layout, "melee/a.c", "table")->action == ChunkAction::InPlace);

    // free region fills in a fixed order: code by unit name, then data, then bss
    const Chunk* new_fn = chunk_named(layout, "melee/a.c", "new_fn");
    CHECK(new_fn->action == ChunkAction::New);
    CHECK_EQ(*new_fn->placed_address, int64_t(0x81700000));
    const Chunk* grown_fn = chunk_named(layout, "melee/b.c", "grown_fn");
    CHECK(grown_fn->action == ChunkAction::InPlace);
    CHECK_EQ(*grown_fn->placed_address, int64_t(0x80003140));
    CHECK_EQ(grown_fn->note_text(), std::string("grew into 0x10 of slack, est grow 4 / patch 5 / reloc 5"));
    const Chunk* new_buf = chunk_named(layout, "melee/a.c", "new_buf");
    CHECK(new_buf->action == ChunkAction::New);
    CHECK_EQ(*new_buf->placed_address, int64_t(0x81700010)); // 8-aligned, after 0x10 bytes of code

    // helper_copy has helper's exact bytes, so refs point at helper instead
    const Chunk* helper_copy = chunk_named(layout, "melee/b.c", "helper_copy");
    CHECK(helper_copy->action == ChunkAction::Alias);
    CHECK_EQ(*helper_copy->placed_address, int64_t(0x80003120));
    CHECK_EQ(helper_copy->note_text(), std::string("= vanilla helper"));

    // nothing reaches dead_fn, orphan_fn changed but lost its last caller, b.c's inline_fn is a duplicate
    CHECK(chunk_named(layout, "melee/a.c", "dead_fn")->action == ChunkAction::Dropped);
    CHECK(chunk_named(layout, "melee/b.c", "orphan_fn")->action == ChunkAction::Dropped);
    CHECK(chunk_named(layout, "melee/b.c", "inline_fn")->action == ChunkAction::Duplicate);
    CHECK(any_line_contains(
        layout.report_lines(), "dropped (unreferenced, absent from vanilla): dead_fn, inline_fn, orphan_fn"));

    CHECK(chunk_named(layout, "melee/a.c", "helper")->action == ChunkAction::Same);
    CHECK(chunk_named(layout, "melee/a.c", "sd_counter")->action == ChunkAction::Same);
    CHECK(chunk_named(layout, "melee/gm/tourney.c", "tourney_fn")->action == ChunkAction::Same);

    const MemImage& image = layout.image();
    CHECK_EQ(image.word(0x80003108), uint32_t(0x48000039)); // bl grown_fn 80003108 -> 80003140 = +0x38
    CHECK_EQ(image.word(0x80400004), uint32_t(0x81700000)); // table[1] = &new_fn
    CHECK_EQ(image.word(0x81700000), uint32_t(0x3C808170)); // lis r4, new_buf@ha
    CHECK_EQ(image.word(0x81700004), uint32_t(0x38840010)); // addi r4, r4, new_buf@l
    CHECK_EQ(image.word(0x80003144), uint32_t(0x4BFFFFDD)); // bl helper: 80003144 -> 80003120 = -0x24
    CHECK_EQ(image.word(0x80003148), uint32_t(0x4BFFFFD9)); // bl helper_copy, which is helper: -0x28
}

TEST(generate_codes) {
    synthetic::ScratchDir scratch;
    Layout layout(mod_checkout(scratch).root);
    GenerateOptions options;
    options.name = "Synthetic Mod";
    options.slippi_ini = SlippiIniSource::none();
    GenerateResult generated = generate(layout, options);

    CHECK(generated.status == GenerateResult::Status::Ok);
    CHECK(generated.reserved_conflicts.empty());

    // main_loop's two changed words are 12 bytes apart: two 04s (2 lines) beat one 06 (3 lines).
    // grown_fn grew in place over dropped orphan_fn: its first two words match vanilla, the
    // next five don't -> one 06. table's one changed word -> an 04. new_fn & the zero fill
    // of new_buf are adjacent in the free region & merge into one write
    const std::string expected_ini =
        "[Gecko]\n"
        "$Synthetic Mod [Decomp2Gecko]\n"
        "20003108 48000019\n" // guard on first changed DOL word: bl helper as vanilla has it
        "04003108 48000039\n" // bl grown_fn: 80003108 -> 80003140
        "04003118 38000007\n"
        "06003148 00000014\n"
        "4BFFFFD9 38600005\n" // bl helper_copy = helper: 80003148 -> 80003120
        "38600006 7C0803A6\n"
        "4E800020 00000000\n"
        "04400004 81700000\n"
        "07700000 00000030\n" // 06 write above 0x81000000 sets low type bit
        "3C808170 38840010\n"
        "4E800020 60000000\n"
        "00000000 00000000\n" // new_buf, 0x20 bytes of zero
        "00000000 00000000\n"
        "00000000 00000000\n"
        "00000000 00000000\n"
        "E2000001 00000000\n"
        "\n"
        "[Gecko_Enabled]\n"
        "$Synthetic Mod\n";

    CHECK_EQ(generated.ini_text, expected_ini);
    CHECK_EQ(generated.codes.gecko_lines, size_t(16));
    CHECK_EQ(generated.codes.in_dol_bytes, int64_t(0x20));
    CHECK_EQ(generated.codes.relocated_bytes, int64_t(0x30));
    CHECK(any_line_contains(generated.report, "gecko lines: 16  in-DOL bytes: 0x20  relocated bytes: 0x30"));
}

TEST(verify_vs_elf) {
    synthetic::ScratchDir scratch;
    synthetic::Checkout checkout = mod_checkout(scratch);
    Layout layout(checkout.root);

    VerifyResult without_elf = verify(layout);
    CHECK(!without_elf.ran);
    CHECK(any_line_contains(without_elf.problems, "main.elf missing; build the decomp first"));

    // the linked ELF shifts everything, only targets have to agree
    checkout.main_elf(linked_main_elf(false));
    VerifyResult verified = verify(layout);
    CHECK(verified.ran);
    // bss isn't compared
    CHECK_EQ(verified.checked, int64_t(5)); // main_loop, table, new_fn, grown_fn, helper_copy
    CHECK_EQ(verified.ok, int64_t(5));
    CHECK(verified.problems.empty());
    CHECK(verified.skipped.empty());

    // one branch in linked grown_fn now points 4 bytes past helper
    checkout.main_elf(linked_main_elf(true));
    VerifyResult mismatched = verify(layout);
    CHECK_EQ(mismatched.ok, int64_t(4));
    CHECK_EQ(mismatched.problems.size(), size_t(1));
    CHECK(contains(mismatched.problems[0], "melee/b.c:.text:grown_fn"));
    CHECK(contains(mismatched.problems[0], "REL24 +0x4 -> helper: elf field 80003124 != elf symbol 80003120"));
}

TEST(generate_slippi_check) {
    synthetic::ScratchDir scratch;
    Layout layout(mod_checkout(scratch).root);

    // a hook on a word grown_fn keeps (fine), a write the mod also makes (conflict), & a hook in
    // helper, which helper_copy aliases onto (conflict: the alias would run slippi's hook too)
    std::filesystem::path ini = scratch.path / "GALE01r2.ini";
    write_file_bytes(ini,
        "[Gecko_Enabled]\n$Slippi Online\n\n[Gecko]\n$Slippi Online [Fizzi]\n"
        "C2003144 00000001\n60000000 00000000\n"
        "04400004 00000000\n"
        "C2003124 00000001\n60000000 00000000\n");
    GenerateOptions options;
    options.name = "Synthetic Mod";
    options.slippi_ini = SlippiIniSource::at(ini);
    GenerateResult generated = generate(layout, options);

    CHECK(generated.status == GenerateResult::Status::Ok);
    CHECK(generated.slippi.ini.has_value());
    CHECK(!generated.slippi.requested_ini_missing);
    CHECK_EQ(generated.slippi.sites.size(), size_t(3));
    CHECK_EQ(generated.slippi.conflicts.size(), size_t(2));
    CHECK_EQ(generated.slippi.conflicts[0],
        std::string("Slippi Online: write at 80400004 overlaps a mod write (80400004-80400008)"));
    CHECK_EQ(generated.slippi.conflicts[1],
        std::string(
            "Slippi Online: hook at 80003124 lands on vanilla bytes helper_copy now shares (the mod aliases onto them)"));
    CHECK_EQ(generated.slippi.notes.size(), size_t(3));
    CHECK(any_line_contains(generated.report, ": 3 sites, 2 conflicts, 3 notes"));

    // w/ slippi's sites known up front the alias isn't made & the conflict disappears
    LayoutOptions avoiding;
    avoiding.avoid = {{0x80003124, 0x80003128}};
    Layout steered(mod_checkout(scratch).root, avoiding);
    CHECK(chunk_named(steered, "melee/b.c", "helper_copy")->action == ChunkAction::New);
    CHECK_EQ(generate(steered, options).slippi.conflicts.size(), size_t(1));

    // a relocated function's vanilla range catches writes that only run into it too
    write_file_bytes(ini,
        "[Gecko_Enabled]\n$Slippi Online\n\n[Gecko]\n$Slippi Online [Fizzi]\n"
        "0600313C 00000008\n60000000 60000000\n");
    LayoutOptions no_patch;
    no_patch.patch_in_place = false;
    Layout relocating(mod_checkout(scratch).root, no_patch);
    GenerateResult straddling = generate(relocating, options);
    CHECK(chunk_named(relocating, "melee/b.c", "grown_fn")->action == ChunkAction::InPlace); // it still fits in place
    CHECK_EQ(straddling.slippi.conflicts.size(), size_t(0));

    options.slippi_ini = SlippiIniSource::at(scratch.path / "missing.ini");
    GenerateResult missing = generate(layout, options);
    CHECK(missing.slippi.requested_ini_missing);
    CHECK(missing.slippi.conflicts.empty());
}

TEST(layout_errors) {
    synthetic::ScratchDir scratch;
    ModOptions options;
    options.grow_small_data = true;
    options.new_small_data = true;
    synthetic::Checkout checkout = write_checkout(scratch, mod_a(options), mod_b(), tourney(true));
    Layout layout(checkout.root);

    // small data is r13 + 16 bits, so it can't move; slippi's list would overwrite the reserved range
    CHECK_EQ(layout.errors().size(), size_t(3));
    CHECK(any_line_contains(layout.errors(), "sd_counter: small-data grew 0x4 -> 0x8; cannot relocate .sdata"));
    CHECK(any_line_contains(layout.errors(), "sd_new: new small-data (.sdata, 0x4 bytes) cannot be relocated"));
    CHECK(any_line_contains(layout.errors(), "tourney_fn: in-place write into reserved range (tournament-mode code"));
    CHECK(any_line_contains(layout.report_lines(), "ERROR: melee/gm/tourney.c:.text:tourney_fn"));

    GenerateOptions generate_options;
    generate_options.name = "Broken";
    GenerateResult generated = generate(layout, generate_options);

    CHECK(generated.status == GenerateResult::Status::LayoutErrors);
    CHECK(generated.ini_text.empty());
}

TEST(layout_free_region_full) {
    synthetic::ScratchDir scratch;
    ModOptions options;
    options.new_buf_size = 0x100000; // whole free region, on top of the relocated code
    synthetic::Checkout checkout = mod_checkout(scratch, options);
    CHECK_THROWS(Layout(checkout.root), LayoutError);
}

TEST(layout_unbuilt) {
    synthetic::ScratchDir scratch;
    synthetic::Checkout checkout(scratch.path / "melee");
    CHECK_THROWS(Layout(checkout.root), MissingFileError); // no vanilla DOL yet
    checkout.vanilla(vanilla_dol(), SYMBOLS, SPLITS);
    CHECK_THROWS(Layout(checkout.root), NotBuiltError); // no config.json: never configured
    checkout.config(UNITS);
    CHECK_THROWS(Layout(checkout.root), NotBuiltError); // configured but no objects
    checkout.object("melee/a", clean_a().build());
    checkout.object("melee/b", clean_b().build());
    checkout.object("melee/gm/tourney", tourney(false).build());
    Layout layout(checkout.root);
    CHECK_EQ(layout.decomp().units.size(), size_t(3));
    CHECK_EQ(layout.decomp().units[2].stem, std::string("melee/gm/tourney"));
    CHECK(layout.decomp().units[2].in_vanilla);
}

TEST(layout_strong_beats_weak) {
    // alpha.c's weak shared_fn matches vanilla, beta.c's strong one differs: the strong copy must win
    const Bytes VANILLA_BODY = words({0x38600001, 0x4E800020, 0x60000000, 0x60000000});
    const Bytes MODDED_BODY = words({0x38600099, 0x4E800020, 0x60000000, 0x60000000});

    const std::string symbols = "shared_fn = .text:0x80003100; // type:function size:0x10 scope:weak\n";
    const std::string splits = "alpha.c:\n"
                               "\t.text       start:0x80003100 end:0x80003110\n\n"
                               "beta.c:\n"
                               "\t.text       start:0x80003110 end:0x80003120\n";
    const std::vector<std::string> units = {"alpha.c", "beta.c"};

    Bytes dol = synthetic::build_dol({{0x80003100, VANILLA_BODY}}, {});

    ElfBuilder alpha;
    alpha.section(".text", CODE_FLAGS, VANILLA_BODY).symbol("shared_fn", ".text", 0x00, 0x10, STT_FUNC, STB_WEAK);

    ElfBuilder beta;
    beta.section(".text", CODE_FLAGS, MODDED_BODY).symbol("shared_fn", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL);

    synthetic::ScratchDir scratch;
    synthetic::Checkout checkout(scratch.path / "melee");
    checkout.vanilla(dol, symbols, splits);
    checkout.config(units);
    checkout.object("alpha", alpha.build());
    checkout.object("beta", beta.build());

    Layout layout(checkout.root);
    const Chunk* alpha_chunk = chunk_named(layout, "alpha.c", "shared_fn");
    const Chunk* beta_chunk = chunk_named(layout, "beta.c", "shared_fn");

    CHECK(beta_chunk->kept);
    CHECK(beta_chunk->changed);
    CHECK(!alpha_chunk->kept);
    CHECK(alpha_chunk->action == ChunkAction::Duplicate);
}

// ---- grown functions patched in place ------------------------------------------------------
//
// .text  80003100  helper (0x10) loop_fn (0x20) check_fn (0x20) guard_fn (0x10)   loop.c
// .data  80400000  table (0xC: &loop_fn, &loop_fn+0xC, &check_fn)
//
// loop_fn: two words inserted before its backwards bne. check_fn: a cmpwi/bne pair inserted, its
// bne aims at a word staying in place & must flip. guard_fn stays, so both islands go far
namespace {

const Bytes LOOP_HELPER = words({0x38600001, 0x4E800020, 0x60000000, 0x60000000});
const Bytes GUARD_BODY = words({0x38600004, 0x4E800020, 0x60000000, 0x60000000});
// mflr / li r3,0 / addi r3,r3,1 / cmpwi r3,5 / bne -8 / bl helper / mtlr / blr
const Bytes LOOP_VANILLA =
    words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x4082FFF8, 0x4BFFFFDD, 0x7C0803A6, 0x4E800020});
// mflr / bl helper / cmpwi r3,0 / beq +8 / li r3,1 / mtlr / blr / nop
const Bytes CHECK_VANILLA =
    words({0x7C0802A6, 0x4BFFFFCD, 0x2C030000, 0x41820008, 0x38600001, 0x7C0803A6, 0x4E800020, 0x60000000});

// object bytes: bl fields left blank for relocation, insertions present
Bytes loop_body(bool modded) {
    if (!modded) {
        return words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x4082FFF8, 0x48000001, 0x7C0803A6, 0x4E800020});
    }
    return words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x38840001, 0x90850000, 0x4082FFF0, 0x48000001,
        0x7C0803A6, 0x4E800020});
}
Bytes check_body(bool modded) {
    if (!modded) {
        return words({0x7C0802A6, 0x48000001, 0x2C030000, 0x41820008, 0x38600001, 0x7C0803A6, 0x4E800020, 0x60000000});
    }
    return words({0x7C0802A6, 0x48000001, 0x2C030000, 0x41820010, 0x2C040000, 0x40820008, 0x38600001, 0x7C0803A6,
        0x4E800020, 0x60000000});
}

const std::string LOOP_SYMBOLS = "helper = .text:0x80003100; // type:function size:0x10 scope:global\n"
                                 "loop_fn = .text:0x80003110; // type:function size:0x20 scope:global\n"
                                 "check_fn = .text:0x80003130; // type:function size:0x20 scope:global\n"
                                 "guard_fn = .text:0x80003150; // type:function size:0x10 scope:global\n"
                                 "table = .data:0x80400000; // type:object size:0xC scope:global\n";
const std::string LOOP_SPLITS = "loop.c:\n"
                                "\t.text       start:0x80003100 end:0x80003160\n"
                                "\t.data       start:0x80400000 end:0x8040000C\n";

ElfBuilder loop_object(bool modded) {
    int64_t loop_size = modded ? 0x28 : 0x20;
    int64_t check_size = modded ? 0x28 : 0x20;
    ElfBuilder object;
    object.section(".text", CODE_FLAGS, LOOP_HELPER + loop_body(modded) + check_body(modded) + GUARD_BODY)
        .section(".data", DATA_FLAGS, words({0, 0, 0}), 8)
        .symbol("helper", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("loop_fn", ".text", 0x10, loop_size, STT_FUNC, STB_GLOBAL)
        .symbol("check_fn", ".text", 0x10 + loop_size, check_size, STT_FUNC, STB_GLOBAL)
        .symbol("guard_fn", ".text", 0x10 + loop_size + check_size, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x00, 0x0C, STT_OBJECT, STB_GLOBAL)
        .reloc(".text", 0x10 + (modded ? 0x1C : 0x14), R_PPC_REL24, "helper")
        .reloc(".text", 0x10 + loop_size + 0x04, R_PPC_REL24, "helper")
        .reloc(".data", 0x00, R_PPC_ADDR32, "loop_fn")
        .reloc(".data", 0x04, R_PPC_ADDR32, "loop_fn", 0xC)
        .reloc(".data", 0x08, R_PPC_ADDR32, "check_fn");
    return object;
}

synthetic::Checkout loop_checkout(const synthetic::ScratchDir& scratch, bool modded) {
    synthetic::Checkout checkout(scratch.path / "melee");
    checkout.vanilla(synthetic::build_dol({{0x80003100, LOOP_HELPER + LOOP_VANILLA + CHECK_VANILLA + GUARD_BODY}},
                         {{0x80400000, words({0x80003110, 0x8000311C, 0x80003130})}}),
        LOOP_SYMBOLS, LOOP_SPLITS);
    checkout.config({"loop.c"});
    checkout.object("loop", loop_object(modded).build());
    return checkout;
}

// the mod as MWLD links it: everything contiguous, loop_fn 0x28 long pushes check_fn to 80003138
//   loop_fn   bne -16: 80003128 -> 80003118            4082FFF0
//             bl helper: 8000312C -> 80003100 = -0x2C  4BFFFFD5
//   check_fn  bl helper: 8000313C -> 80003100 = -0x3C  4BFFFFC5
//             beq +16: 80003144 -> 80003154 (mtlr)     41820010
//             bne +8:  8000314C -> 80003154            40820008
Bytes loop_main_elf(bool corrupt_bne = false) {
    uint32_t bne = corrupt_bne ? 0x4082FFF4 : 0x4082FFF0; // -12 aims at the cmpwi instead of the addi
    Bytes text = LOOP_HELPER +
        words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x38840001, 0x90850000, bne, 0x4BFFFFD5, 0x7C0803A6,
            0x4E800020}) +
        words({0x7C0802A6, 0x4BFFFFC5, 0x2C030000, 0x41820010, 0x2C040000, 0x40820008, 0x38600001, 0x7C0803A6,
            0x4E800020, 0x60000000}) +
        GUARD_BODY;
    ElfBuilder elf;
    elf.section(".text", CODE_FLAGS, text, 4, 0x80003100)
        .section(".data", DATA_FLAGS, words({0x80003110, 0x8000311C, 0x80003138}), 8, 0x80400000)
        .symbol("helper", ".text", 0x80003100, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("loop_fn", ".text", 0x80003110, 0x28, STT_FUNC, STB_GLOBAL)
        .symbol("check_fn", ".text", 0x80003138, 0x28, STT_FUNC, STB_GLOBAL)
        .symbol("guard_fn", ".text", 0x80003160, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x80400000, 0x0C, STT_OBJECT, STB_GLOBAL)
        .absolute("_SDA_BASE_", 0x804DB6A0)
        .absolute("_SDA2_BASE_", 0x804DF9E0);
    return elf.build(true);
}

} // namespace

TEST(layout_patches_grown_functions) {
    synthetic::ScratchDir scratch;
    synthetic::Checkout checkout = loop_checkout(scratch, true);
    Layout layout(checkout.root);

    CHECK_EQ(layout.report_lines()[0],
        std::string(
            "units: 1  chunks: 5  unchanged: 3  in-place: 0  patched: 2  relocated: 0  new: 0  aliased: 0  dropped: 0"));
    CHECK_EQ(layout.report_lines()[1], std::string("free region: 0x81700000 used 0x24 of 0x100000"));
    CHECK(layout.errors().empty());

    // loop_fn: the cmpwi before the insertion moves into the island (a plain instruction beats
    // the bne after it), the hook takes its slot, the island returns to the bne
    const Chunk* loop_fn = chunk_named(layout, "loop.c", "loop_fn");
    CHECK(loop_fn->action == ChunkAction::Patched);
    CHECK_EQ(*loop_fn->placed_address, int64_t(0x80003110));
    CHECK_EQ(loop_fn->note_text(), std::string("island 0x10 far, est grow - / patch 4 / reloc 8"));
    const PatchPlan* loop_plan = layout.patch_plan(*loop_fn);
    CHECK(loop_plan != nullptr);
    CHECK_EQ(loop_plan->island_base, int64_t(0x81700000));
    CHECK_EQ(*layout.placed_address_of(*loop_fn, 0xC), int64_t(0x81700000)); // the moved cmpwi
    CHECK_EQ(*layout.placed_address_of(*loop_fn, 0x18), int64_t(0x80003120)); // the bne
    CHECK(loop_plan->dead_vanilla.empty());

    // check_fn: the li after the insertion moves, the inserted bne is flipped & followed by a b,
    // the vanilla nop slot at the end is dead
    const Chunk* check_fn = chunk_named(layout, "loop.c", "check_fn");
    CHECK(check_fn->action == ChunkAction::Patched);
    const PatchPlan* check_plan = layout.patch_plan(*check_fn);
    CHECK(check_plan != nullptr);
    CHECK_EQ(check_plan->island_base, int64_t(0x81700010));
    CHECK_EQ(check_plan->island.size(), size_t(5));
    CHECK_EQ(check_plan->dead_vanilla.size(), size_t(1));
    CHECK_EQ(check_plan->dead_vanilla[0].first, int64_t(0x8000314C));

    const MemImage& image = layout.image();
    CHECK_EQ(image.word(0x8000311C), uint32_t(0x496FCEE4)); // hook: b 81700000 = +0x16FCEE4
    CHECK_EQ(image.word(0x80003120), uint32_t(0x4082FFF8)); // bne back to 80003118, as vanilla
    CHECK_EQ(image.word(0x80003124), uint32_t(0x4BFFFFDD)); // bl helper, as vanilla
    CHECK_EQ(image.word(0x81700000), uint32_t(0x2C030005));
    CHECK_EQ(image.word(0x81700004), uint32_t(0x38840001));
    CHECK_EQ(image.word(0x81700008), uint32_t(0x90850000));
    CHECK_EQ(image.word(0x8170000C), uint32_t(0x4A903114)); // b 80003120 from 8170000C = -0x16FCEEC
    CHECK_EQ(image.word(0x80003134), uint32_t(0x4BFFFFCD)); // bl helper, as vanilla
    CHECK_EQ(image.word(0x8000313C), uint32_t(0x41820008)); // beq +8 to the mtlr, as vanilla
    CHECK_EQ(image.word(0x80003140), uint32_t(0x496FCED0)); // hook: b 81700010
    CHECK_EQ(image.word(0x81700010), uint32_t(0x2C040000));
    CHECK_EQ(image.word(0x81700014), uint32_t(0x41820008)); // bne flipped to beq, over the helper
    CHECK_EQ(image.word(0x81700018), uint32_t(0x4A90312C)); // b 80003144 (mtlr) from 81700018
    CHECK_EQ(image.word(0x8170001C), uint32_t(0x38600001));
    CHECK_EQ(image.word(0x81700020), uint32_t(0x4A903124)); // b 80003144 from 81700020
    CHECK_EQ(image.word(0x8000314C), uint32_t(0x60000000)); // dead slot keeps its vanilla nop
    CHECK_EQ(image.word(0x80400004), uint32_t(0x81700000)); // table[1] followed the cmpwi
    // read back in compiled order: the bl resolved, the bne re-encoded for its in-place slot
    CHECK_EQ(layout.logical_bytes(*loop_fn),
        Bytes(*loop_fn->data())
            .replace(0x18, 4, packed_u32_be(0x4082FFF8))
            .replace(0x1C, 4, packed_u32_be(0x4BFFFFDD)));

    GenerateOptions options;
    options.name = "Loop";
    options.slippi_ini = SlippiIniSource::none();
    GenerateResult generated = generate(layout, options);
    CHECK(generated.status == GenerateResult::Status::Ok);
    const std::string expected_ini = "[Gecko]\n"
                                     "$Loop [Decomp2Gecko]\n"
                                     "2000311C 2C030005\n" // guard: the first hook's slot still holds its cmpwi
                                     "0400311C 496FCEE4\n"
                                     "04003140 496FCED0\n"
                                     "04400004 81700000\n"
                                     "07700000 00000024\n" // both islands, back to back
                                     "2C030005 38840001\n"
                                     "90850000 4A903114\n"
                                     "2C040000 41820008\n"
                                     "4A90312C 38600001\n"
                                     "4A903124 00000000\n"
                                     "E2000001 00000000\n"
                                     "\n"
                                     "[Gecko_Enabled]\n"
                                     "$Loop\n";
    CHECK_EQ(generated.ini_text, expected_ini);
    CHECK_EQ(generated.codes.gecko_lines, size_t(11));
    CHECK_EQ(generated.codes.in_dol_bytes, int64_t(0xC));
    CHECK_EQ(generated.codes.relocated_bytes, int64_t(0x24));

    // the decomp's link is contiguous; targets still have to agree
    checkout.main_elf(loop_main_elf());
    VerifyResult verified = verify(layout);
    CHECK(verified.ran);
    CHECK_EQ(verified.checked, int64_t(2));
    CHECK_EQ(verified.ok, int64_t(2));
    CHECK(verified.problems.empty());

    // a label branch whose target disagrees w/ the decomp's own link is caught
    checkout.main_elf(loop_main_elf(true));
    VerifyResult mismatched = verify(layout);
    CHECK_EQ(mismatched.ok, int64_t(1));
    CHECK_EQ(mismatched.problems.size(), size_t(1));
    CHECK(contains(mismatched.problems[0], "loop.c:.text:loop_fn"));
    CHECK(contains(mismatched.problems[0], "branch +0x18 -> +0x8: elf word 4082FFF4 targets something else"));
}

TEST(layout_patch_can_be_switched_off) {
    synthetic::ScratchDir scratch;
    LayoutOptions options;
    options.patch_in_place = false;
    Layout layout(loop_checkout(scratch, true).root, options);
    CHECK_EQ(layout.report_lines()[0],
        std::string(
            "units: 1  chunks: 5  unchanged: 3  in-place: 0  patched: 0  relocated: 2  new: 0  aliased: 0  dropped: 0"));
    CHECK_EQ(layout.report_lines()[1], std::string("free region: 0x81700000 used 0x50 of 0x100000"));
    CHECK(chunk_named(layout, "loop.c", "loop_fn")->action == ChunkAction::Relocated);
    CHECK_EQ(layout.image().word(0x80400004), uint32_t(0x8170000C)); // table[1] = relocated loop_fn + 0xC
    CHECK_EQ(layout.image().word(0x80400008), uint32_t(0x81700028)); // table[2] = relocated check_fn
}

TEST(generate_slippi_check_patched) {
    synthetic::ScratchDir scratch;
    Layout layout(loop_checkout(scratch, true).root);

    // hook on a kept word: fine. on check_fn's dead nop: dead code. on our hook word: collides
    std::filesystem::path ini = scratch.path / "GALE01r2.ini";
    // & a string write that starts on check_fn's kept blr but runs into the dead slot behind it
    write_file_bytes(ini,
        "[Gecko_Enabled]\n$Slippi Online\n\n[Gecko]\n$Slippi Online [Fizzi]\n"
        "C2003118 00000001\n60000000 00000000\n"
        "C200314C 00000001\n60000000 00000000\n"
        "C2003140 00000001\n60000000 00000000\n"
        "06003148 00000008\n4E800020 60000000\n");
    GenerateOptions options;
    options.name = "Loop";
    options.slippi_ini = SlippiIniSource::at(ini);
    GenerateResult generated = generate(layout, options);
    CHECK(generated.status == GenerateResult::Status::Ok);
    CHECK_EQ(generated.slippi.sites.size(), size_t(4));
    CHECK_EQ(generated.slippi.conflicts.size(), size_t(3));
    CHECK_EQ(generated.slippi.conflicts[0],
        std::string("Slippi Online: hook at 8000314C is inside check_fn, which the mod patches around it (the Slippi "
                    "hook would land in dead code)"));
    CHECK_EQ(generated.slippi.conflicts[1],
        std::string("Slippi Online: hook at 80003140 overlaps a mod write (80003140-80003144)"));
    CHECK_EQ(generated.slippi.conflicts[2],
        std::string("Slippi Online: write at 80003148 is inside check_fn, which the mod patches around it (the Slippi "
                    "write would land in dead code)"));
    CHECK_EQ(generated.slippi.notes.size(), size_t(4));
}

// ---- data: growing into padding, aliasing onto vanilla, & the store guard --------------------
//
// .text  80003100  user_fn (0x18): lis r3,tbl@ha / lwz-or-stw r0,tbl@l(r3) / lis+addi r4,name / blr / nop
// .data  80400000  other (0x10: 4 x &user_fn) tbl (0x10: &user_fn, 0, 0, 0) name (0x4: "abc")
//        section ends at 80400030, so name has 0xC bytes of padding behind it
namespace {

const std::string DATA_SYMBOLS = "user_fn = .text:0x80003100; // type:function size:0x18 scope:global\n"
                                 "other = .data:0x80400000; // type:object size:0x10 scope:global\n"
                                 "tbl = .data:0x80400010; // type:object size:0x10 scope:global\n"
                                 "name = .data:0x80400020; // type:object size:0x4 scope:global\n";
const std::string DATA_SPLITS = "data.c:\n"
                                "\t.text       start:0x80003100 end:0x80003118\n"
                                "\t.data       start:0x80400000 end:0x80400030\n";

enum class Touch {
    Load, // lwz r0, tbl@l(r3)
    Store, // stw r0, tbl@l(r3)
    AddressTaken, // addi r3, r3, tbl@l
    None, // user_fn doesn't mention tbl (lwz keeps its linked bytes)
    StoreOther, // stw r0, other@l(r3): the alias destination gets written
    HighHalfOnly, // lis r3, tbl@ha & a store w/ a literal displacement: no @l names the object
    UpdateLoad, // lwzu r0, tbl@l(r3): the address stays in r3 afterwards
    LoadListedTable, // lwz r0, ftData_CharacterStateTables@l(r3) from code the mod changed
    SpareHigh, // a proper lis/lwz pair of tbl, plus a second lis tbl@ha feeding a literal-offset store
};

// the mod fills tbl w/ pointers so it equals other, & lengthens name
ElfBuilder data_object(bool modded, Touch touch) {
    uint32_t access = touch == Touch::Load || touch == Touch::LoadListedTable ? 0x80030000
        : touch == Touch::AddressTaken                                        ? 0x38630000
        : touch == Touch::UpdateLoad                                          ? 0x84030000
                                                                              : 0x90030000;
    Bytes text = words({0x3C600000, access, 0x3C800000, 0x38840000, 0x4E800020, 0x60000000});
    if (touch == Touch::LoadListedTable) {
        text =
            words({0x3C600000, access, 0x3C800000, 0x38840000, 0x4E800020, 0x38A00001}); // the trailing nop became a li
    } else if (touch == Touch::SpareHigh) {
        text = words({0x3C600000, 0x80030000, 0x3C800000, 0x90040010, 0x4E800020, 0x60000000});
    }
    if (touch == Touch::None) {
        text = words({0x3C608040, 0x80030010, 0x3C800000, 0x38840000, 0x4E800020, 0x60000000});
    } else if (touch == Touch::HighHalfOnly) {
        text = words({0x3C600000, 0x90030010, 0x3C800000, 0x38840000, 0x4E800020, 0x60000000});
    }
    Bytes name = modded ? Bytes("abcdef\0", 7) : Bytes("abc\0", 4);
    ElfBuilder object;
    object.section(".text", CODE_FLAGS, text)
        .section(".data", DATA_FLAGS, words({0, 0, 0, 0, 0, 0, 0, 0}) + name, 8)
        .symbol("user_fn", ".text", 0x00, 0x18, STT_FUNC, STB_GLOBAL)
        .symbol("other", ".data", 0x00, 0x10, STT_OBJECT, STB_GLOBAL)
        .symbol("tbl", ".data", 0x10, 0x10, STT_OBJECT, STB_GLOBAL)
        .symbol("name", ".data", 0x20, int64_t(name.size()), STT_OBJECT, STB_GLOBAL)
        .reloc(".data", 0x00, R_PPC_ADDR32, "user_fn")
        .reloc(".data", 0x04, R_PPC_ADDR32, "user_fn")
        .reloc(".data", 0x08, R_PPC_ADDR32, "user_fn")
        .reloc(".data", 0x0C, R_PPC_ADDR32, "user_fn")
        .reloc(".data", 0x10, R_PPC_ADDR32, "user_fn");
    if (touch != Touch::SpareHigh) {
        object.reloc(".text", 0x0A, R_PPC_ADDR16_HA, "name").reloc(".text", 0x0E, R_PPC_ADDR16_LO, "name");
    }
    if (touch == Touch::StoreOther) {
        object.reloc(".text", 0x02, R_PPC_ADDR16_HA, "other").reloc(".text", 0x06, R_PPC_ADDR16_LO, "other");
    } else if (touch == Touch::LoadListedTable) {
        object.undefined("ftData_CharacterStateTables")
            .reloc(".text", 0x02, R_PPC_ADDR16_HA, "ftData_CharacterStateTables")
            .reloc(".text", 0x06, R_PPC_ADDR16_LO, "ftData_CharacterStateTables");
    } else if (touch == Touch::HighHalfOnly) {
        object.reloc(".text", 0x02, R_PPC_ADDR16_HA, "tbl");
    } else if (touch == Touch::SpareHigh) {
        object.reloc(".text", 0x02, R_PPC_ADDR16_HA, "tbl")
            .reloc(".text", 0x06, R_PPC_ADDR16_LO, "tbl")
            .reloc(".text", 0x0A, R_PPC_ADDR16_HA, "tbl");
    } else if (touch != Touch::None) {
        object.reloc(".text", 0x02, R_PPC_ADDR16_HA, "tbl").reloc(".text", 0x06, R_PPC_ADDR16_LO, "tbl");
    }
    if (modded) {
        object.reloc(".data", 0x14, R_PPC_ADDR32, "user_fn")
            .reloc(".data", 0x18, R_PPC_ADDR32, "user_fn")
            .reloc(".data", 0x1C, R_PPC_ADDR32, "user_fn");
    }
    return object;
}

// a second unit w/ a pointer table aimed at tbl: vouched for when named like melee's, else not
const std::string TABLE_SYMBOLS =
    "ftData_CharacterStateTables = .data:0x80400030; // type:object size:0x4 scope:global\n";
const std::string TABLE_SPLITS = "melee/ft/ftdata.c:\n"
                                 "\t.data       start:0x80400030 end:0x80400034\n";
const std::string PLAIN_SYMBOLS = "plain_tables = .data:0x80400030; // type:object size:0x4 scope:global\n";
const std::string PLAIN_SPLITS = "plain.c:\n"
                                 "\t.data       start:0x80400030 end:0x80400034\n";

ElfBuilder table_object(const std::string& name) {
    ElfBuilder object;
    object.section(".data", DATA_FLAGS, words({0}), 8)
        .symbol(name, ".data", 0x00, 0x04, STT_OBJECT, STB_GLOBAL)
        .undefined("tbl")
        .reloc(".data", 0x00, R_PPC_ADDR32, "tbl");
    return object;
}

enum class Via {
    Nothing,
    ListedTable, // melee/ft/ftdata.c: ftData_CharacterStateTables
    PlainTable, // plain.c: plain_tables
};

synthetic::Checkout data_checkout(const synthetic::ScratchDir& scratch, bool modded, Touch touch,
    Via via = Via::Nothing, bool ghost_unit = false, bool dirty_padding = false) {
    synthetic::Checkout checkout(scratch.path / "melee");
    // user_fn as vanilla links it: lis r3,0x8040 / l/stw r0,0x10(r3) / lis r4,0x8040 / addi r4,r4,0x20
    uint32_t access = touch == Touch::Store || touch == Touch::HighHalfOnly ? 0x90030010
        : touch == Touch::Load                                              ? 0x80030010
        : touch == Touch::UpdateLoad                                        ? 0x84030010
        : touch == Touch::StoreOther                                        ? 0x90030000
        : touch == Touch::LoadListedTable                                   ? 0x80030030
                                                                            : 0x38630010;
    Bytes text = words({0x3C608040, access, 0x3C808040, 0x38840020, 0x4E800020, 0x60000000});
    if (touch == Touch::SpareHigh) {
        text = words({0x3C608040, 0x80030010, 0x3C808040, 0x90040010, 0x4E800020, 0x60000000});
    }
    Bytes padding = dirty_padding ? Bytes(2, '\0') + Bytes(1, '\x01') + Bytes(9, '\0') : Bytes(0xC, '\0');
    Bytes data =
        words({0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100, 0, 0, 0}) + Bytes("abc\0", 4) + padding;
    std::vector<synthetic::DolSection> data_sections = {{0x80400000, data}};
    std::string symbols = DATA_SYMBOLS, splits = DATA_SPLITS;
    std::vector<std::string> units = {"data.c"};
    if (via != Via::Nothing) {
        data_sections.push_back({0x80400030, words({0x80400010})});
        symbols += via == Via::ListedTable ? TABLE_SYMBOLS : PLAIN_SYMBOLS;
        splits += via == Via::ListedTable ? TABLE_SPLITS : PLAIN_SPLITS;
        units.push_back(via == Via::ListedTable ? "melee/ft/ftdata.c" : "plain.c");
    }
    if (ghost_unit) {
        units.push_back("ghost.c"); // configured, never built
    }
    checkout.vanilla(synthetic::build_dol({{0x80003100, text}}, data_sections), symbols, splits);
    checkout.config(units);
    checkout.object("data", data_object(modded, touch).build());
    if (via == Via::ListedTable) {
        checkout.object("melee/ft/ftdata", table_object("ftData_CharacterStateTables").build());
    } else if (via == Via::PlainTable) {
        checkout.object("plain", table_object("plain_tables").build());
    }
    return checkout;
}

} // namespace

TEST(layout_aliases_read_only_data) {
    synthetic::ScratchDir scratch;
    Layout layout(data_checkout(scratch, true, Touch::Load).root);

    // tbl now equals other: rewriting it is a 3-line 06, repointing user_fn's lis/addi is 2 lines.
    // name grew into its padding
    CHECK_EQ(layout.report_lines()[0],
        std::string(
            "units: 1  chunks: 4  unchanged: 2  in-place: 1  patched: 0  relocated: 0  new: 0  aliased: 1  dropped: 0"));
    const Chunk* tbl = chunk_named(layout, "data.c", "tbl");
    CHECK(tbl->action == ChunkAction::Alias);
    CHECK_EQ(*tbl->placed_address, int64_t(0x80400000));
    CHECK_EQ(tbl->note_text(), std::string("= vanilla other (data: only ever loaded)"));
    const Chunk* name = chunk_named(layout, "data.c", "name");
    CHECK(name->action == ChunkAction::InPlace);
    CHECK_EQ(name->note_text(), std::string("grew into 0xC of slack"));
    CHECK_EQ(layout.image().word(0x80003104), uint32_t(0x80030000)); // lwz r0, other@l(r3)
    CHECK_EQ(layout.image().read(0x80400020, 7), Bytes("abcdef\0", 7));

    GenerateOptions options;
    options.name = "Data";
    options.slippi_ini = SlippiIniSource::none();
    GenerateResult generated = generate(layout, options);
    CHECK(generated.status == GenerateResult::Status::Ok);
    // the lwz's low half, then name's two words
    CHECK_EQ(generated.ini_text,
        std::string("[Gecko]\n$Data [Decomp2Gecko]\n"
                    "20003104 80030010\n"
                    "04003104 80030000\n"
                    "06400020 00000008\n"
                    "61626364 65660000\n"
                    "E2000001 00000000\n"
                    "\n[Gecko_Enabled]\n$Data\n"));
}

TEST(layout_never_aliases_stored_to_data) {
    synthetic::ScratchDir scratch;
    Layout layout(data_checkout(scratch, true, Touch::Store).root);
    // user_fn stores into tbl, so tbl must stay its own object
    const Chunk* tbl = chunk_named(layout, "data.c", "tbl");
    CHECK(tbl->action == ChunkAction::InPlace);
    CHECK_EQ(*tbl->placed_address, int64_t(0x80400010));
    CHECK_EQ(layout.image().word(0x80003104), uint32_t(0x90030010)); // stw r0, tbl@l(r3), untouched
    CHECK_EQ(layout.image().word(0x8040001C), uint32_t(0x80003100));
    CHECK(!layout.read_only_reason(*tbl).has_value());

    // the address in a register could go anywhere
    Layout taken(data_checkout(scratch, true, Touch::AddressTaken).root);
    CHECK(chunk_named(taken, "data.c", "tbl")->action == ChunkAction::InPlace);
    CHECK(!taken.read_only_reason(*chunk_named(taken, "data.c", "tbl")).has_value());

    // a lone @ha is an address in a register too, the store behind it names nothing
    Layout high(data_checkout(scratch, true, Touch::HighHalfOnly).root);
    CHECK(chunk_named(high, "data.c", "tbl")->action == ChunkAction::InPlace);
    CHECK(!high.read_only_reason(*chunk_named(high, "data.c", "tbl")).has_value());

    // an update-form load leaves the address behind in its base register
    Layout updating(data_checkout(scratch, true, Touch::UpdateLoad).root);
    CHECK(chunk_named(updating, "data.c", "tbl")->action == ChunkAction::InPlace);
    CHECK(!updating.read_only_reason(*chunk_named(updating, "data.c", "tbl")).has_value());

    // a proper pair doesn't excuse a second, unpaired @ha in the same function
    Layout spare(data_checkout(scratch, true, Touch::SpareHigh).root);
    CHECK(chunk_named(spare, "data.c", "tbl")->action == ChunkAction::InPlace);
    CHECK(!spare.read_only_reason(*chunk_named(spare, "data.c", "tbl")).has_value());

    LayoutOptions options;
    options.alias_named_data = false;
    Layout conservative(data_checkout(scratch, true, Touch::Load).root, options);
    CHECK(chunk_named(conservative, "data.c", "tbl")->action == ChunkAction::InPlace);
}

TEST(layout_read_only_through_listed_tables) {
    // address taken by user_fn: no, even though a vouched table also points at it
    synthetic::ScratchDir scratch;
    Layout mixed(data_checkout(scratch, true, Touch::AddressTaken, Via::ListedTable).root);
    CHECK(!mixed.read_only_reason(*chunk_named(mixed, "data.c", "tbl")).has_value());

    // only a pointer in a listed table: read-only through the table
    Layout listed(data_checkout(scratch, true, Touch::None, Via::ListedTable).root);
    const Chunk* tbl = chunk_named(listed, "data.c", "tbl");
    CHECK(tbl->action == ChunkAction::Alias);
    CHECK_EQ(*listed.read_only_reason(*tbl),
        std::string("referenced only from read-only table ftData_CharacterStateTables"));
    const Chunk* tables = chunk_named(listed, "melee/ft/ftdata.c", "ftData_CharacterStateTables");
    CHECK_EQ(*listed.read_only_reason(*tables),
        std::string(
            "listed in the game config: per-character dispatch or resource table in ftdata.c, audited: only ever read through"));
    CHECK_EQ(listed.image().word(0x80400030), uint32_t(0x80400000)); // the table now points at other

    // the same pointer in a table nobody vouches for
    Layout plain(data_checkout(scratch, true, Touch::None, Via::PlainTable).root);
    CHECK(chunk_named(plain, "data.c", "tbl")->action == ChunkAction::InPlace);
    CHECK(!plain.read_only_reason(*chunk_named(plain, "data.c", "tbl")).has_value());

    // the audit covers vanilla code only: a changed function reading the table voids its vouch
    Layout touched(data_checkout(scratch, true, Touch::LoadListedTable, Via::ListedTable).root);
    CHECK(chunk_named(touched, "data.c", "user_fn")->action == ChunkAction::InPlace);
    CHECK(!touched.read_only_reason(*chunk_named(touched, "melee/ft/ftdata.c", "ftData_CharacterStateTables"))
            .has_value());
    CHECK(!touched.read_only_reason(*chunk_named(touched, "data.c", "tbl")).has_value());
    CHECK(chunk_named(touched, "data.c", "tbl")->action == ChunkAction::InPlace);
}

// ---- growing into a departing neighbour, & two identical new tables -----------------------
//
// .data  80400000  tbl_a (0x10) tbl_b (0x10) sep (0x10, unchanged) tbl_c (0x10)   tabs.c
//        80400040  ftData_CharacterStateTables (0xC: &tbl_a, &tbl_b, &tbl_c)                     melee/ft/ftdata.c
//
// all three grow to 6 pointers: tbl_b leaves & tbl_a grows into its slot; tbl_c can't grow
// (sep stays, section ends) & equals tbl_b, so one copy serves both
namespace {

const std::string TABS_SYMBOLS =
    "user_fn = .text:0x80003100; // type:function size:0x10 scope:global\n"
    "tbl_a = .data:0x80400000; // type:object size:0x10 scope:global\n"
    "tbl_b = .data:0x80400010; // type:object size:0x10 scope:global\n"
    "sep = .data:0x80400020; // type:object size:0x10 scope:global\n"
    "tbl_c = .data:0x80400030; // type:object size:0x10 scope:global\n"
    "ftData_CharacterStateTables = .data:0x80400040; // type:object size:0xC scope:global\n";
const std::string TABS_SPLITS = "tabs.c:\n"
                                "\t.text       start:0x80003100 end:0x80003110\n"
                                "\t.data       start:0x80400000 end:0x80400040\n\n"
                                "melee/ft/ftdata.c:\n"
                                "\t.data       start:0x80400040 end:0x8040004C\n";

synthetic::Checkout tabs_checkout(const synthetic::ScratchDir& scratch) {
    synthetic::Checkout checkout(scratch.path / "melee");
    // user_fn: lis r3,tbl_c@ha / lwz r0,tbl_c@l(r3) / li r3,1 / blr. the mod changes the li
    Bytes text = words({0x3C608040, 0x80030030, 0x38600001, 0x4E800020});
    // tbl_a: four pointers. tbl_b, tbl_c: one pointer & zeros. sep: constants
    Bytes data = words({0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100, 0, 0, 0, 0x11, 0x22, 0x33, 0x44,
        0x80003100, 0, 0, 0});
    checkout.vanilla(synthetic::build_dol({{0x80003100, text}},
                         {{0x80400000, data}, {0x80400040, words({0x80400000, 0x80400010, 0x80400030})}}),
        TABS_SYMBOLS, TABS_SPLITS);
    checkout.config({"tabs.c", "melee/ft/ftdata.c"});

    ElfBuilder tabs;
    tabs.section(".text", CODE_FLAGS, words({0x3C600000, 0x80030000, 0x38600002, 0x4E800020}))
        .section(".data", DATA_FLAGS, Bytes(0x18 * 3, '\0') + words({0x11, 0x22, 0x33, 0x44}), 8)
        .symbol("user_fn", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("tbl_a", ".data", 0x00, 0x18, STT_OBJECT, STB_GLOBAL)
        .symbol("tbl_b", ".data", 0x18, 0x18, STT_OBJECT, STB_GLOBAL)
        .symbol("tbl_c", ".data", 0x30, 0x18, STT_OBJECT, STB_GLOBAL)
        .symbol("sep", ".data", 0x48, 0x10, STT_OBJECT, STB_GLOBAL);
    for (int64_t offset = 0; offset < 0x48; offset += 4) {
        tabs.reloc(".data", offset, R_PPC_ADDR32, "user_fn");
    }
    tabs.reloc(".text", 0x02, R_PPC_ADDR16_HA, "tbl_c").reloc(".text", 0x06, R_PPC_ADDR16_LO, "tbl_c");
    checkout.object("tabs", tabs.build());

    ElfBuilder ftdata;
    ftdata.section(".data", DATA_FLAGS, words({0, 0, 0}), 8)
        .symbol("ftData_CharacterStateTables", ".data", 0x00, 0x0C, STT_OBJECT, STB_GLOBAL)
        .undefined("tbl_a")
        .undefined("tbl_b")
        .undefined("tbl_c")
        .reloc(".data", 0x00, R_PPC_ADDR32, "tbl_a")
        .reloc(".data", 0x04, R_PPC_ADDR32, "tbl_b")
        .reloc(".data", 0x08, R_PPC_ADDR32, "tbl_c");
    checkout.object("melee/ft/ftdata", ftdata.build());
    return checkout;
}

// the mod as linked: three 0x18 tables back to back, then sep; ftdata's table in its own unit
Bytes tabs_main_elf() {
    Bytes six = words({0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100});
    ElfBuilder elf;
    elf.section(".text", CODE_FLAGS, words({0x3C608040, 0x80030030, 0x38600002, 0x4E800020}), 4, 0x80003100)
        .section(".data", DATA_FLAGS, six + six + six + words({0x11, 0x22, 0x33, 0x44}), 8, 0x80400000)
        .section(".data2", DATA_FLAGS, words({0x80400000, 0x80400018, 0x80400030}), 8, 0x80400060)
        .symbol("user_fn", ".text", 0x80003100, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("tbl_a", ".data", 0x80400000, 0x18, STT_OBJECT, STB_GLOBAL)
        .symbol("tbl_b", ".data", 0x80400018, 0x18, STT_OBJECT, STB_GLOBAL)
        .symbol("tbl_c", ".data", 0x80400030, 0x18, STT_OBJECT, STB_GLOBAL)
        .symbol("sep", ".data", 0x80400048, 0x10, STT_OBJECT, STB_GLOBAL)
        .symbol("ftData_CharacterStateTables", ".data2", 0x80400060, 0x0C, STT_OBJECT, STB_GLOBAL)
        .absolute("_SDA_BASE_", 0x804DB6A0)
        .absolute("_SDA2_BASE_", 0x804DF9E0);
    return elf.build(true);
}

} // namespace

TEST(layout_grows_into_departing_data_and_dedups_tables) {
    synthetic::ScratchDir scratch;
    synthetic::Checkout checkout = tabs_checkout(scratch);
    Layout layout(checkout.root);

    CHECK_EQ(layout.report_lines()[0],
        std::string(
            "units: 2  chunks: 6  unchanged: 2  in-place: 2  patched: 0  relocated: 1  new: 0  aliased: 1  dropped: 0"));
    CHECK_EQ(layout.report_lines()[1], std::string("free region: 0x81700000 used 0x18 of 0x100000"));

    const Chunk* tbl_a = chunk_named(layout, "tabs.c", "tbl_a");
    CHECK(tbl_a->action == ChunkAction::InPlace);
    CHECK_EQ(tbl_a->note_text(), std::string("grew into 0x10 of slack"));
    const Chunk* tbl_b = chunk_named(layout, "tabs.c", "tbl_b");
    CHECK(tbl_b->action == ChunkAction::Relocated);
    CHECK_EQ(*tbl_b->placed_address, int64_t(0x81700000));
    const Chunk* tbl_c = chunk_named(layout, "tabs.c", "tbl_c");
    CHECK(tbl_c->action == ChunkAction::Alias);
    CHECK(!tbl_c->kept);
    CHECK(tbl_c->replaced_by == tbl_b);
    CHECK_EQ(tbl_c->note_text(),
        std::string(
            "= tbl_b (identical; data: only ever loaded, or reached through read-only table ftData_CharacterStateTables)"));
    CHECK(any_line_contains(layout.report_lines(), "alias     tabs.c"));

    const MemImage& image = layout.image();
    CHECK_EQ(image.read(0x80400000, 0x18),
        Bytes(words({0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100})));
    CHECK_EQ(image.word(0x80400020), uint32_t(0x11)); // sep untouched
    CHECK_EQ(image.word(0x80400040), uint32_t(0x80400000));
    CHECK_EQ(image.word(0x80400044), uint32_t(0x81700000));
    CHECK_EQ(image.word(0x80400048), uint32_t(0x81700000)); // tbl_c is tbl_b now
    CHECK_EQ(image.word(0x80003100), uint32_t(0x3C608170)); // user_fn loads from tbl_b as well
    CHECK_EQ(image.word(0x80003104), uint32_t(0x80030000));
    CHECK_EQ(image.read(0x81700000, 0x18),
        Bytes(words({0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100, 0x80003100})));

    GenerateOptions options;
    options.name = "Tabs";
    options.slippi_ini = SlippiIniSource::none();
    GenerateResult generated = generate(layout, options);
    CHECK(generated.status == GenerateResult::Status::Ok);
    // user_fn's lis/lwz/li, tbl_a's sixth word (over tbl_b's old second word), the two repointed
    // table entries, tbl_b
    CHECK_EQ(generated.ini_text,
        std::string("[Gecko]\n$Tabs [Decomp2Gecko]\n"
                    "20003100 3C608040\n"
                    "06003100 0000000C\n"
                    "3C608170 80030000\n"
                    "38600002 00000000\n"
                    "04400014 80003100\n"
                    "06400044 00000008\n"
                    "81700000 81700000\n"
                    "07700000 00000018\n"
                    "80003100 80003100\n"
                    "80003100 80003100\n"
                    "80003100 80003100\n"
                    "E2000001 00000000\n"
                    "\n[Gecko_Enabled]\n$Tabs\n"));

    // main.elf keeps tbl_c as its own object: user_fn's field must name it there & tbl_b here
    checkout.main_elf(tabs_main_elf());
    VerifyResult verified = verify(layout);
    CHECK(verified.ran);
    CHECK_EQ(verified.checked, int64_t(3)); // user_fn, tbl_a, tbl_b
    CHECK_EQ(verified.ok, int64_t(3));
    CHECK(verified.problems.empty());
}

// ---- a duplicate symbol's owner is the copy we keep -------------------------------------------
//
// alpha.c's weak shared_tbl vs beta.c's strong one, stored into by writer_fn. alpha's tbl equals
// it & is read-only through a listed table, but shared_tbl's storage is written
namespace {

const std::string DUP_SYMBOLS =
    "writer_fn = .text:0x80003100; // type:function size:0x10 scope:global\n"
    "shared_tbl = .data:0x80400000; // type:object size:0x10 scope:weak\n"
    "tbl = .data:0x80400010; // type:object size:0x10 scope:global\n"
    "ftData_CharacterStateTables = .data:0x80400020; // type:object size:0x4 scope:global\n";
const std::string DUP_SPLITS = "alpha.c:\n"
                               "\t.data       start:0x80400000 end:0x80400020\n\n"
                               "beta.c:\n"
                               "\t.text       start:0x80003100 end:0x80003110\n\n"
                               "melee/ft/ftdata.c:\n"
                               "\t.data       start:0x80400020 end:0x80400024\n";

synthetic::Checkout dup_checkout(const synthetic::ScratchDir& scratch) {
    synthetic::Checkout checkout(scratch.path / "melee");
    Bytes shared = words({0x11, 0x22, 0x33, 0x44});
    // writer_fn: lis r3,shared_tbl@ha / stw r0,shared_tbl@l(r3) / blr / nop
    checkout.vanilla(synthetic::build_dol({{0x80003100, words({0x3C608040, 0x90030000, 0x4E800020, 0x60000000})}},
                         {{0x80400000, shared + words({0, 0, 0, 0})}, {0x80400020, words({0x80400010})}}),
        DUP_SYMBOLS, DUP_SPLITS);
    checkout.config({"alpha.c", "beta.c", "melee/ft/ftdata.c"});

    ElfBuilder alpha;
    alpha.section(".data", DATA_FLAGS, shared + shared, 8)
        .symbol("shared_tbl", ".data", 0x00, 0x10, STT_OBJECT, STB_WEAK)
        .symbol("tbl", ".data", 0x10, 0x10, STT_OBJECT, STB_GLOBAL);
    checkout.object("alpha", alpha.build());

    ElfBuilder beta;
    beta.section(".text", CODE_FLAGS, words({0x3C600000, 0x90030000, 0x4E800020, 0x60000000}))
        .section(".data", DATA_FLAGS, shared, 8)
        .symbol("writer_fn", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("shared_tbl", ".data", 0x00, 0x10, STT_OBJECT, STB_GLOBAL)
        .reloc(".text", 0x02, R_PPC_ADDR16_HA, "shared_tbl")
        .reloc(".text", 0x06, R_PPC_ADDR16_LO, "shared_tbl");
    checkout.object("beta", beta.build());

    ElfBuilder ftdata;
    ftdata.section(".data", DATA_FLAGS, words({0}), 8)
        .symbol("ftData_CharacterStateTables", ".data", 0x00, 0x04, STT_OBJECT, STB_GLOBAL)
        .undefined("tbl")
        .reloc(".data", 0x00, R_PPC_ADDR32, "tbl");
    checkout.object("melee/ft/ftdata", ftdata.build());
    return checkout;
}

} // namespace

TEST(layout_duplicate_owner_is_the_kept_copy) {
    synthetic::ScratchDir scratch;
    Layout layout(dup_checkout(scratch).root);
    CHECK_EQ(layout.report_lines()[0],
        std::string(
            "units: 3  chunks: 5  unchanged: 3  in-place: 1  patched: 0  relocated: 0  new: 0  aliased: 0  dropped: 1"));
    CHECK(chunk_named(layout, "alpha.c", "shared_tbl")->action == ChunkAction::Duplicate);
    CHECK(!layout.read_only_reason(*chunk_named(layout, "beta.c", "shared_tbl")).has_value());
    const Chunk* tbl = chunk_named(layout, "alpha.c", "tbl");
    CHECK(layout.read_only_reason(*tbl).has_value());
    CHECK(tbl->action == ChunkAction::InPlace); // shared_tbl's bytes match, its storage is written
}

TEST(layout_incomplete_build_never_aliases_data) {
    // a unit w/o an object could store into anything: the read-only proof is off
    synthetic::ScratchDir scratch;
    Layout layout(data_checkout(scratch, true, Touch::Load, Via::Nothing, true).root);
    CHECK(!layout.options().alias_named_data);
    CHECK(chunk_named(layout, "data.c", "tbl")->action == ChunkAction::InPlace);
    CHECK(any_line_contains(layout.report_lines(),
        "skipped units (1 objects not found; no data aliasing, no reuse of vacated space): ghost.c"));
}

TEST(layout_padding_must_be_zero) {
    // a nonzero byte behind name is data nobody named, so name can't grow over it
    synthetic::ScratchDir scratch;
    Layout layout(data_checkout(scratch, true, Touch::Load, Via::Nothing, false, true).root);
    const Chunk* name = chunk_named(layout, "data.c", "name");
    CHECK(name->action == ChunkAction::Relocated);
    CHECK_EQ(*name->placed_address, int64_t(0x81700000));
    CHECK_EQ(layout.image().read(0x80400024, 3), Bytes("\0\0\x01", 3)); // untouched
}

// ---- growing code: in place, local island or far island -------------------------------------
//
// .text  80003100  helper (0x10) big_fn (0x38) dead_fn (0x10) guard_fn (0x10)   grow.c
// .data  80400000  table (0x8: &big_fn, &guard_fn)
//
// big_fn: two words inserted early. dead_fn grows but loses its last reference: its slot is free,
// & an island there beats rewriting big_fn's tail. twin_fn (dead_fn's bytes) on that slot = far island
namespace {

const std::string GROW_SYMBOLS = "helper = .text:0x80003100; // type:function size:0x10 scope:global\n"
                                 "big_fn = .text:0x80003110; // type:function size:0x38 scope:global\n"
                                 "dead_fn = .text:0x80003148; // type:function size:0x10 scope:global\n"
                                 "guard_fn = .text:0x80003158; // type:function size:0x10 scope:global\n"
                                 "table = .data:0x80400000; // type:object size:0x8 scope:global\n";
const std::string GROW_SPLITS = "grow.c:\n"
                                "\t.text       start:0x80003100 end:0x80003168\n"
                                "\t.data       start:0x80400000 end:0x80400008\n";
// mflr / li / addi / cmpwi / bne -8 / li r5,1..6 / bl helper / mtlr / blr
const Bytes BIG_VANILLA = words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x4082FFF8, 0x38A00001, 0x38A00002,
    0x38A00003, 0x38A00004, 0x38A00005, 0x38A00006, 0x4BFFFFC5, 0x7C0803A6, 0x4E800020});
const Bytes BIG_FRESH = words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x38840001, 0x90850000, 0x4082FFF0,
    0x38A00001, 0x38A00002, 0x38A00003, 0x38A00004, 0x38A00005, 0x38A00006, 0x48000001, 0x7C0803A6, 0x4E800020});
const Bytes DEAD_VANILLA = words({0x38600003, 0x4E800020, 0x60000000, 0x60000000});
const Bytes DEAD_FRESH = words({0x38600007, 0x38600008, 0x4E800020, 0x60000000, 0x60000000, 0x60000000});

synthetic::Checkout grow_checkout(const synthetic::ScratchDir& scratch, bool twin) {
    synthetic::Checkout checkout(scratch.path / "melee");
    checkout.vanilla(synthetic::build_dol({{0x80003100, LOOP_HELPER + BIG_VANILLA + DEAD_VANILLA + GUARD_BODY}},
                         {{0x80400000, words({0x80003110, 0x80003158})}}),
        GROW_SYMBOLS, GROW_SPLITS);
    checkout.config({"grow.c"});
    ElfBuilder object;
    object
        .section(
            ".text", CODE_FLAGS, LOOP_HELPER + BIG_FRESH + DEAD_FRESH + GUARD_BODY + (twin ? DEAD_VANILLA : Bytes()))
        .section(".data", DATA_FLAGS, words({0, 0}), 8)
        .symbol("helper", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("big_fn", ".text", 0x10, 0x40, STT_FUNC, STB_GLOBAL)
        .symbol("dead_fn", ".text", 0x50, 0x18, STT_FUNC, STB_GLOBAL)
        .symbol("guard_fn", ".text", 0x68, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x00, 0x08, STT_OBJECT, STB_GLOBAL)
        .reloc(".text", 0x44, R_PPC_REL24, "helper")
        .reloc(".data", 0x00, R_PPC_ADDR32, "big_fn");
    if (twin) {
        object.symbol("twin_fn", ".text", 0x78, 0x10, STT_FUNC, STB_GLOBAL)
            .reloc(".data", 0x04, R_PPC_ADDR32, "twin_fn");
    } else {
        object.reloc(".data", 0x04, R_PPC_ADDR32, "guard_fn");
    }
    checkout.object("grow", object.build());
    return checkout;
}

// the mod as linked: big_fn 0x40 pushes dead_fn to 80003150, dead_fn 0x18 pushes guard_fn to 80003168
//   big_fn  bne -16: 80003128 -> 80003118            4082FFF0
//           bl helper: 80003144 -> 80003100 = -0x44  4BFFFFBD
Bytes grow_main_elf() {
    Bytes text = LOOP_HELPER +
        words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x38840001, 0x90850000, 0x4082FFF0, 0x38A00001,
            0x38A00002, 0x38A00003, 0x38A00004, 0x38A00005, 0x38A00006, 0x4BFFFFBD, 0x7C0803A6, 0x4E800020}) +
        DEAD_FRESH + GUARD_BODY;
    ElfBuilder elf;
    elf.section(".text", CODE_FLAGS, text, 4, 0x80003100)
        .section(".data", DATA_FLAGS, words({0x80003110, 0x80003168}), 8, 0x80400000)
        .symbol("helper", ".text", 0x80003100, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("big_fn", ".text", 0x80003110, 0x40, STT_FUNC, STB_GLOBAL)
        .symbol("dead_fn", ".text", 0x80003150, 0x18, STT_FUNC, STB_GLOBAL)
        .symbol("guard_fn", ".text", 0x80003168, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x80400000, 0x08, STT_OBJECT, STB_GLOBAL)
        .absolute("_SDA_BASE_", 0x804DB6A0)
        .absolute("_SDA2_BASE_", 0x804DF9E0);
    return elf.build(true);
}

} // namespace

TEST(layout_local_island_beats_growing_in_place) {
    synthetic::ScratchDir scratch;
    synthetic::Checkout checkout = grow_checkout(scratch, false);
    Layout layout(checkout.root);

    // growing in place rewrites the eleven shifted words (a 7-line 06); the island in dead_fn's
    // slot is a hook plus 16 bytes; a whole relocation is 8 lines plus the table fixup
    const Chunk* big_fn = chunk_named(layout, "grow.c", "big_fn");
    CHECK(big_fn->action == ChunkAction::Patched);
    CHECK_EQ(big_fn->note_text(), std::string("island 0x10 local, est grow 7 / patch 4 / reloc 10"));
    const PatchPlan* plan = layout.patch_plan(*big_fn);
    CHECK(!plan->island_far);
    CHECK_EQ(plan->island_base, int64_t(0x80003148));
    CHECK_EQ(layout.free_used(), int64_t(0));
    CHECK(chunk_named(layout, "grow.c", "dead_fn")->action == ChunkAction::Dropped);

    const MemImage& image = layout.image();
    CHECK_EQ(image.word(0x8000311C), uint32_t(0x4800002C)); // hook: b 80003148
    CHECK_EQ(image.word(0x80003120), uint32_t(0x4082FFF8)); // bne, as vanilla
    CHECK_EQ(image.word(0x8000313C), uint32_t(0x4BFFFFC5)); // bl helper, as vanilla
    CHECK_EQ(image.word(0x80003148), uint32_t(0x2C030005));
    CHECK_EQ(image.word(0x8000314C), uint32_t(0x38840001));
    CHECK_EQ(image.word(0x80003150), uint32_t(0x90850000));
    CHECK_EQ(image.word(0x80003154), uint32_t(0x4BFFFFCC)); // b 80003120 from 80003154 = -0x34

    GenerateOptions options;
    options.name = "Grow";
    options.slippi_ini = SlippiIniSource::none();
    GenerateResult generated = generate(layout, options);
    CHECK(generated.status == GenerateResult::Status::Ok);
    CHECK_EQ(generated.ini_text,
        std::string("[Gecko]\n$Grow [Decomp2Gecko]\n"
                    "2000311C 2C030005\n"
                    "0400311C 4800002C\n"
                    "06003148 00000010\n"
                    "2C030005 38840001\n"
                    "90850000 4BFFFFCC\n"
                    "E2000001 00000000\n"
                    "\n[Gecko_Enabled]\n$Grow\n"));
    CHECK_EQ(generated.codes.relocated_bytes, int64_t(0));

    checkout.main_elf(grow_main_elf());
    VerifyResult verified = verify(layout);
    CHECK_EQ(verified.checked, int64_t(1));
    CHECK_EQ(verified.ok, int64_t(1));
    CHECK(verified.problems.empty());
}

TEST(layout_slack_holding_an_alias_stays_live) {
    // twin_fn has dead_fn's vanilla bytes & aliases onto its slot, so big_fn's island goes far.
    // it took guard_fn's table entry, which must not make it guard_fn
    synthetic::ScratchDir scratch;
    Layout layout(grow_checkout(scratch, true).root);
    const Chunk* twin_fn = chunk_named(layout, "grow.c", "twin_fn");
    CHECK(twin_fn->is_new());
    CHECK(chunk_named(layout, "grow.c", "guard_fn")->action == ChunkAction::Same);
    CHECK(twin_fn->action == ChunkAction::Alias);
    CHECK_EQ(*twin_fn->placed_address, int64_t(0x80003148));
    const Chunk* big_fn = chunk_named(layout, "grow.c", "big_fn");
    CHECK(big_fn->action == ChunkAction::Patched);
    CHECK_EQ(big_fn->note_text(), std::string("island 0x10 far, est grow - / patch 4 / reloc 9"));
    CHECK(layout.patch_plan(*big_fn)->island_far);
    CHECK_EQ(layout.patch_plan(*big_fn)->island_base, int64_t(0x81700000));
    CHECK_EQ(layout.image().word(0x80003148), uint32_t(0x38600003)); // twin_fn's bytes, not an island
    CHECK_EQ(layout.image().word(0x80400004), uint32_t(0x80003148)); // table[1] = twin_fn = dead_fn's slot
}

// ---- a bc from another function into a patched one (REL14) ----------------------------------
//
// .text  80003100  helper (0x10) target_fn (0x20) jumper_fn (0x10)   rel14.c
// .data  80400000  table (0x8: &target_fn, &jumper_fn)
namespace {

const std::string REL14_SYMBOLS = "helper = .text:0x80003100; // type:function size:0x10 scope:global\n"
                                  "target_fn = .text:0x80003110; // type:function size:0x20 scope:global\n"
                                  "jumper_fn = .text:0x80003130; // type:function size:0x10 scope:global\n"
                                  "table = .data:0x80400000; // type:object size:0x8 scope:global\n";
const std::string REL14_SPLITS = "rel14.c:\n"
                                 "\t.text       start:0x80003100 end:0x80003140\n"
                                 "\t.data       start:0x80400000 end:0x80400008\n";

// target_fn grows like loop_fn. accept: vanilla jumper_fn has beq target_fn+8 (the addi, stays put).
// reject: the mod adds beq target_fn+0xC (the cmpwi, moves) & jumper_fn outgrows its slot
synthetic::Checkout rel14_checkout(const synthetic::ScratchDir& scratch, bool reject) {
    synthetic::Checkout checkout(scratch.path / "melee");
    // vanilla: cmpwi / beq +8 into target_fn (80003134 -> 80003118 = -0x1C) / blr / nop, or w/o the beq
    Bytes jumper_vanilla = reject ? words({0x2C030000, 0x38600000, 0x4E800020, 0x60000000})
                                  : words({0x2C030000, 0x4182FFE4, 0x4E800020, 0x60000000});
    checkout.vanilla(synthetic::build_dol({{0x80003100, LOOP_HELPER + LOOP_VANILLA + jumper_vanilla}},
                         {{0x80400000, words({0x80003110, 0x80003130})}}),
        REL14_SYMBOLS, REL14_SPLITS);
    checkout.config({"rel14.c"});
    Bytes jumper_fresh = reject ? words({0x2C030000, 0x41820000, 0x38600000, 0x4E800020, 0x60000000, 0x60000000})
                                : words({0x2C030000, 0x41820000, 0x4E800020, 0x60000000});
    ElfBuilder object;
    object.section(".text", CODE_FLAGS, LOOP_HELPER + loop_body(true) + jumper_fresh)
        .section(".data", DATA_FLAGS, words({0, 0}), 8)
        .symbol("helper", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("target_fn", ".text", 0x10, 0x28, STT_FUNC, STB_GLOBAL)
        .symbol("jumper_fn", ".text", 0x38, int64_t(jumper_fresh.size()), STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x00, 0x08, STT_OBJECT, STB_GLOBAL)
        .reloc(".text", 0x2C, R_PPC_REL24, "helper")
        .reloc(".text", 0x3C, R_PPC_REL14, "target_fn", reject ? 0xC : 0x8)
        .reloc(".data", 0x00, R_PPC_ADDR32, "target_fn")
        .reloc(".data", 0x04, R_PPC_ADDR32, "jumper_fn");
    checkout.object("rel14", object.build());
    return checkout;
}

} // namespace

TEST(layout_rel14_into_a_patched_function) {
    synthetic::ScratchDir scratch;
    Layout accepted(rel14_checkout(scratch, false).root);
    CHECK(chunk_named(accepted, "rel14.c", "target_fn")->action == ChunkAction::Patched);
    CHECK(chunk_named(accepted, "rel14.c", "jumper_fn")->action == ChunkAction::Same);
    CHECK_EQ(accepted.image().word(0x80003134), uint32_t(0x4182FFE4)); // still reaches the addi

    // the cmpwi would move out of a bc's reach: no plan, both functions move instead & the
    // bc reaches its target in the free region
    Layout rejected(rel14_checkout(scratch, true).root);
    const Chunk* target_fn = chunk_named(rejected, "rel14.c", "target_fn");
    CHECK(target_fn->action == ChunkAction::Relocated);
    CHECK(contains(target_fn->note_text(), "patch: a reference into the function has no home"));
    CHECK_EQ(*target_fn->placed_address, int64_t(0x81700000));
    const Chunk* jumper_fn = chunk_named(rejected, "rel14.c", "jumper_fn");
    CHECK(jumper_fn->action == ChunkAction::Relocated);
    CHECK_EQ(*jumper_fn->placed_address, int64_t(0x81700028));
    CHECK_EQ(rejected.image().word(0x8170002C), uint32_t(0x4182FFE0)); // beq 8170002C -> 8170000C = -0x20
}

TEST(layout_alias_destination_must_be_immutable) {
    // tbl is read-only through the listed table, but other, where its bytes already are, gets
    // written by user_fn: sharing would let those writes leak into tbl
    synthetic::ScratchDir scratch;
    Layout written(data_checkout(scratch, true, Touch::StoreOther, Via::ListedTable).root);
    CHECK(written.read_only_reason(*chunk_named(written, "data.c", "tbl")).has_value());
    CHECK(!written.read_only_reason(*chunk_named(written, "data.c", "other")).has_value());
    CHECK(chunk_named(written, "data.c", "tbl")->action == ChunkAction::InPlace);

    // bytes some other code patches (slippi's) are off limits too
    LayoutOptions options;
    options.avoid = {{0x80400004, 0x80400008}};
    Layout patched(data_checkout(scratch, true, Touch::Load).root, options);
    CHECK(chunk_named(patched, "data.c", "tbl")->action == ChunkAction::InPlace);
}

// ---- a function whose relocation nobody can resolve keeps vanilla bytes by position ---------
//
// .text  80003100  helper (0x10) fn (0x28)   unres.c        .data  80400000  table (0x4: &fn)
// fn loads &mystery, a symbol nobody defines: vanilla's field bytes only work while fn keeps its shape
namespace {

const std::string UNRES_SYMBOLS = "helper = .text:0x80003100; // type:function size:0x10 scope:global\n"
                                  "fn = .text:0x80003110; // type:function size:0x28 scope:global\n"
                                  "table = .data:0x80400000; // type:object size:0x4 scope:global\n";
const std::string UNRES_SPLITS = "unres.c:\n"
                                 "\t.text       start:0x80003100 end:0x80003138\n"
                                 "\t.data       start:0x80400000 end:0x80400004\n";

// grown: two words inserted before the lis/addi. else: only the li's value changes
synthetic::Checkout unres_checkout(const synthetic::ScratchDir& scratch, bool grown) {
    synthetic::Checkout checkout(scratch.path / "melee");
    // mflr / li / addi / cmpwi / bne -8 / lis r5,mystery@ha / addi r5,r5,mystery@l / bl helper (-0x2C) / mtlr / blr
    Bytes vanilla = words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x4082FFF8, 0x3CA08040, 0x38A50010,
        0x4BFFFFD5, 0x7C0803A6, 0x4E800020});
    checkout.vanilla(synthetic::build_dol({{0x80003100, LOOP_HELPER + vanilla}}, {{0x80400000, words({0x80003110})}}),
        UNRES_SYMBOLS, UNRES_SPLITS);
    checkout.config({"unres.c"});
    Bytes fresh = grown ? words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x38840001, 0x90850000, 0x4082FFF0,
                              0x3CA00000, 0x38A50000, 0x48000001, 0x7C0803A6, 0x4E800020})
                        : words({0x7C0802A6, 0x38600007, 0x38630001, 0x2C030005, 0x4082FFF8, 0x3CA00000, 0x38A50000,
                              0x48000001, 0x7C0803A6, 0x4E800020});
    int64_t lis = grown ? 0x1C : 0x14;
    ElfBuilder object;
    object.section(".text", CODE_FLAGS, LOOP_HELPER + fresh)
        .section(".data", DATA_FLAGS, words({0}), 8)
        .symbol("helper", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("fn", ".text", 0x10, int64_t(fresh.size()), STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x00, 0x04, STT_OBJECT, STB_GLOBAL)
        .undefined("mystery")
        .reloc(".text", 0x10 + lis + 2, R_PPC_ADDR16_HA, "mystery")
        .reloc(".text", 0x10 + lis + 6, R_PPC_ADDR16_LO, "mystery")
        .reloc(".text", 0x10 + lis + 8, R_PPC_REL24, "helper")
        .reloc(".data", 0x00, R_PPC_ADDR32, "fn");
    checkout.object("unres", object.build());
    return checkout;
}

} // namespace

TEST(layout_unresolved_fields_need_the_vanilla_shape) {
    synthetic::ScratchDir scratch;
    // same shape: the field's vanilla halves are where the fresh field is
    Layout same(unres_checkout(scratch, false).root);
    const Chunk* fn = chunk_named(same, "unres.c", "fn");
    CHECK(fn->action == ChunkAction::InPlace);
    CHECK_EQ(same.image().word(0x80003114), uint32_t(0x38600007));
    CHECK_EQ(same.image().word(0x80003124), uint32_t(0x3CA08040));
    CHECK_EQ(same.image().word(0x80003128), uint32_t(0x38A50010));
    CHECK(any_line_contains(same.report_lines(), "linker-provided symbols copied from vanilla: mystery"));

    // grown: neither a patch (words move) nor a relocation (offsets shift) can keep them
    try {
        Layout grown(unres_checkout(scratch, true).root);
        throw testing::Failure("expected the grown layout to fail");
    } catch (const LayoutError& error) {
        CHECK(contains(error.what(), "fn references unknown symbol mystery and changed shape"));
    }
}

// ---- a patched function's bc must not aim at something about to move -----------------------
//
// .text  80002FF0  mover_fn (0x10, inside the reserved codehandler range)   rel14b.c
//        80003100  helper (0x10) target_fn (0x20)
// .data  80400000  table (0x8: &target_fn, &mover_fn)
//
// mover_fn grows inside a reserved range, so it relocates. target_fn grows too & bc's to mover_fn:
// patched in place it couldn't reach the free region, so it moves as well, within reach
namespace {

const std::string REL14B_SYMBOLS = "mover_fn = .text:0x80002FF0; // type:function size:0x10 scope:global\n"
                                   "helper = .text:0x80003100; // type:function size:0x10 scope:global\n"
                                   "target_fn = .text:0x80003110; // type:function size:0x20 scope:global\n"
                                   "table = .data:0x80400000; // type:object size:0x8 scope:global\n";
const std::string REL14B_SPLITS = "rel14b.c:\n"
                                  "\t.text       start:0x80002FF0 end:0x80003130\n"
                                  "\t.data       start:0x80400000 end:0x80400008\n";

synthetic::Checkout rel14b_checkout(const synthetic::ScratchDir& scratch) {
    synthetic::Checkout checkout(scratch.path / "melee");
    // target_fn as vanilla: loop_fn w/ beq mover_fn (80003124 -> 80002FF0 = -0x134) instead of bl helper
    Bytes target_vanilla =
        words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x4082FFF8, 0x4182FECC, 0x7C0803A6, 0x4E800020});
    Bytes mover_vanilla = words({0x38600003, 0x4E800020, 0x60000000, 0x60000000});
    checkout.vanilla(synthetic::build_dol({{0x80002FF0, mover_vanilla}, {0x80003100, LOOP_HELPER + target_vanilla}},
                         {{0x80400000, words({0x80003110, 0x80002FF0})}}),
        REL14B_SYMBOLS, REL14B_SPLITS);
    checkout.config({"rel14b.c"});
    Bytes mover_fresh = words({0x38600003, 0x38800001, 0x4E800020, 0x60000000, 0x60000000, 0x60000000});
    Bytes target_fresh = words({0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x38840001, 0x90850000, 0x4082FFF0,
        0x41820000, 0x7C0803A6, 0x4E800020});
    ElfBuilder object;
    object.section(".text", CODE_FLAGS, mover_fresh + LOOP_HELPER + target_fresh)
        .section(".data", DATA_FLAGS, words({0, 0}), 8)
        .symbol("mover_fn", ".text", 0x00, 0x18, STT_FUNC, STB_GLOBAL)
        .symbol("helper", ".text", 0x18, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("target_fn", ".text", 0x28, 0x28, STT_FUNC, STB_GLOBAL)
        .symbol("table", ".data", 0x00, 0x08, STT_OBJECT, STB_GLOBAL)
        .reloc(".text", 0x28 + 0x1C, R_PPC_REL14, "mover_fn")
        .reloc(".data", 0x00, R_PPC_ADDR32, "target_fn")
        .reloc(".data", 0x04, R_PPC_ADDR32, "mover_fn");
    checkout.object("rel14b", object.build());
    return checkout;
}

} // namespace

TEST(layout_rel14_to_a_moving_target_is_not_patched) {
    synthetic::ScratchDir scratch;
    Layout layout(rel14b_checkout(scratch).root);
    const Chunk* mover_fn = chunk_named(layout, "rel14b.c", "mover_fn");
    CHECK(mover_fn->action == ChunkAction::Relocated);
    CHECK(contains(mover_fn->note_text(), "patch: reserved range"));
    CHECK_EQ(*mover_fn->placed_address, int64_t(0x81700000));
    const Chunk* target_fn = chunk_named(layout, "rel14b.c", "target_fn");
    CHECK(target_fn->action == ChunkAction::Relocated);
    CHECK(contains(target_fn->note_text(), "patch: bc to mover_fn, which may move"));
    CHECK_EQ(*target_fn->placed_address, int64_t(0x81700018));
    CHECK_EQ(layout.image().word(0x81700034), uint32_t(0x4182FFCC)); // beq 81700034 -> 81700000
    CHECK_EQ(layout.image().word(0x80400000), uint32_t(0x81700018));
    CHECK_EQ(layout.image().word(0x80400004), uint32_t(0x81700000));
}

// ---- an alias must not land on bytes other code owns at runtime ---------------------------
//
// .text  80003100  fn_a (0x10)  &  a lookalike of the new twin either at 80003110 or inside the
// reserved tournament-mode range at 80190B00       resv.c
// .data  80400000  keep (0x4: &fn_a, the mod points it at twin)
namespace {

synthetic::Checkout reserved_checkout(const synthetic::ScratchDir& scratch, bool lookalike_reserved) {
    synthetic::Checkout checkout(scratch.path / "melee");
    Bytes fn_a = words({0x38600001, 0x4E800020, 0x60000000, 0x60000000});
    Bytes lookalike = words({0x38600009, 0x4E800020, 0x60000000, 0x60000000});
    int64_t lookalike_at = lookalike_reserved ? 0x80190B00 : 0x80003110;
    std::string symbols = "fn_a = .text:0x80003100; // type:function size:0x10 scope:global\n"
                          "lookalike = .text:0x" +
        hex_upper(lookalike_at, 8) +
        "; // type:function size:0x10 scope:global\n"
        "keep = .data:0x80400000; // type:object size:0x4 scope:global\n";
    std::string splits = lookalike_reserved ? "resv.c:\n\t.text       start:0x80003100 end:0x80003110\n"
                                              "\t.data       start:0x80400000 end:0x80400004\n\n"
                                              "melee/gm/tourney.c:\n\t.text       start:0x80190B00 end:0x80190B10\n"
                                            : "resv.c:\n\t.text       start:0x80003100 end:0x80003120\n"
                                              "\t.data       start:0x80400000 end:0x80400004\n";
    std::vector<synthetic::DolSection> text = lookalike_reserved
        ? std::vector<synthetic::DolSection>{{0x80003100, fn_a}, {0x80190B00, lookalike}}
        : std::vector<synthetic::DolSection>{{0x80003100, fn_a + lookalike}};
    checkout.vanilla(synthetic::build_dol(text, {{0x80400000, words({0x80003100})}}), symbols, splits);
    std::vector<std::string> units = {"resv.c"};
    if (lookalike_reserved) {
        units.push_back("melee/gm/tourney.c");
    }
    checkout.config(units);

    ElfBuilder object;
    object.section(".text", CODE_FLAGS, fn_a + lookalike)
        .section(".data", DATA_FLAGS, words({0}), 8)
        .symbol("fn_a", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("twin", ".text", 0x10, 0x10, STT_FUNC, STB_GLOBAL)
        .symbol("keep", ".data", 0x00, 0x04, STT_OBJECT, STB_GLOBAL)
        .reloc(".data", 0x00, R_PPC_ADDR32, "twin");
    checkout.object("resv", object.build());
    if (lookalike_reserved) {
        ElfBuilder tourney;
        tourney.section(".text", CODE_FLAGS, lookalike).symbol("lookalike", ".text", 0x00, 0x10, STT_FUNC, STB_GLOBAL);
        checkout.object("melee/gm/tourney", tourney.build());
    }
    return checkout;
}

} // namespace

TEST(layout_never_aliases_into_reserved_ranges) {
    synthetic::ScratchDir scratch;
    Layout plain(reserved_checkout(scratch, false).root);
    const Chunk* twin = chunk_named(plain, "resv.c", "twin");
    CHECK(twin->action == ChunkAction::Alias);
    CHECK_EQ(*twin->placed_address, int64_t(0x80003110));

    // same bytes inside the tournament-mode range: slippi may overwrite them, so twin is written out
    Layout reserved(reserved_checkout(scratch, true).root);
    const Chunk* kept_out = chunk_named(reserved, "resv.c", "twin");
    CHECK(kept_out->action == ChunkAction::New);
    CHECK_EQ(*kept_out->placed_address, int64_t(0x81700000));
    CHECK_EQ(reserved.image().word(0x80400000), uint32_t(0x81700000));
}
