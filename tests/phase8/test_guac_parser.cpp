#include "core/guac_parser.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace proxy;

// ═════════════════════════════════════════════════════════════════════════════
// GuacParser 파싱 — 정상 경로
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 1: 완전한 명령어 한 개 파싱 ─────────────────────────────────────
//
// 와이어 포맷 "3.img,1.0,4.Over;" → opcode="img", args=["0","Over"]
TEST(GuacParserParsing, CompleteInstruction_ParsesOpcodeAndArgs) {
    GuacParser p;
    p.feed("3.img,1.0,4.Over;");
    ASSERT_TRUE(p.has_instruction());
    auto instr = p.next_instruction();
    EXPECT_EQ(instr.opcode, "img");
    ASSERT_EQ(instr.args.size(), 2u);
    EXPECT_EQ(instr.args[0], "0");
    EXPECT_EQ(instr.args[1], "Over");
    EXPECT_FALSE(p.has_instruction());
}

// ── 시나리오 2: 한 번의 feed()에 여러 명령어 공급 ─────────────────────────────
//
// "3.img;3.end;" → 큐에 두 개 대기, 순서대로 꺼낼 수 있어야 한다.
TEST(GuacParserParsing, MultipleInstructionsInOneFeed_AllQueued) {
    GuacParser p;
    p.feed("3.img;3.end;");
    EXPECT_TRUE(p.has_instruction());
    EXPECT_EQ(p.next_instruction().opcode, "img");
    EXPECT_TRUE(p.has_instruction());
    EXPECT_EQ(p.next_instruction().opcode, "end");
    EXPECT_FALSE(p.has_instruction());
}

// ── 시나리오 3: 청크 분할 공급 ────────────────────────────────────────────────
//
// TCP는 스트리밍이므로 명령어가 여러 조각으로 도착할 수 있다.
// "3.im" + "g;" 두 번의 feed()로 하나의 명령어를 복원해야 한다.
TEST(GuacParserParsing, ChunkedFeed_ReconstructsInstruction) {
    GuacParser p;
    p.feed("3.im");
    EXPECT_FALSE(p.has_instruction());
    p.feed("g;");
    ASSERT_TRUE(p.has_instruction());
    EXPECT_EQ(p.next_instruction().opcode, "img");
}

// ── 시나리오 4: 빈 arg ─────────────────────────────────────────────────────────
//
// "0." = 길이 0인 element → 빈 문자열로 파싱되어야 한다.
TEST(GuacParserParsing, EmptyArg_ParsedAsEmptyString) {
    GuacParser p;
    p.feed("3.foo,0.,3.bar;");
    ASSERT_TRUE(p.has_instruction());
    auto instr = p.next_instruction();
    ASSERT_EQ(instr.args.size(), 2u);
    EXPECT_EQ(instr.args[0], "");
    EXPECT_EQ(instr.args[1], "bar");
}

// ── 시나리오 5: 공급 전후 has_instruction() ────────────────────────────────────
//
// 빈 파서는 has_instruction() == false, 완전한 명령어 공급 후 true.
TEST(GuacParserParsing, HasInstruction_FalseBeforeFeedTrueAfter) {
    GuacParser p;
    EXPECT_FALSE(p.has_instruction());
    p.feed("4.size;");
    EXPECT_TRUE(p.has_instruction());
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacParser 파싱 — 오류 경로
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 6: length 필드에 비숫자 → runtime_error ─────────────────────────
//
// "!" 같은 비숫자가 length 자리에 오면 즉시 파싱 오류가 발생해야 한다.
TEST(GuacParserError, NonDigitInLengthField_Throws) {
    GuacParser p;
    EXPECT_THROW(p.feed("!.img;"), std::runtime_error);
}

// ── 시나리오 7: 구분자 자리에 비구분자 → runtime_error ────────────────────────
//
// element 읽기 완료 후 ','나 ';' 대신 다른 문자가 오면 파싱 오류.
TEST(GuacParserError, InvalidSeparator_Throws) {
    GuacParser p;
    EXPECT_THROW(p.feed("3.img|"), std::runtime_error);
}

// ── 시나리오 8: length 없이 '.' 시작 → runtime_error ─────────────────────────
//
// length 필드가 비어있는 상태에서 '.'이 오면 파싱 오류.
TEST(GuacParserError, EmptyLengthBeforeDot_Throws) {
    GuacParser p;
    EXPECT_THROW(p.feed(".img;"), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacParser 직렬화
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 9: serialize() — 와이어 포맷 정확성 ─────────────────────────────
//
// {opcode="img", args=["0","Over"]} → "3.img,1.0,4.Over;"
TEST(GuacParserSerialize, BasicInstruction_ProducesWireFormat) {
    GuacInstruction instr;
    instr.opcode = "img";
    instr.args   = {"0", "Over"};
    EXPECT_EQ(GuacParser::serialize(instr), "3.img,1.0,4.Over;");
}

// ── 시나리오 10: opcode 빈 문자열 → invalid_argument ─────────────────────────
//
// opcode 없이 직렬화를 시도하면 즉시 invalid_argument가 던져져야 한다.
TEST(GuacParserSerialize, EmptyOpcode_ThrowsInvalidArgument) {
    GuacInstruction instr;
    instr.opcode = "";
    EXPECT_THROW(GuacParser::serialize(instr), std::invalid_argument);
}

// ── 시나리오 11: 직렬화 → feed() 왕복 — 원본과 동일 ──────────────────────────
//
// serialize()한 결과를 다시 feed()하면 원본 instruction이 복원되어야 한다.
TEST(GuacParserSerialize, RoundTrip_FeedAfterSerialize_RestoresOriginal) {
    GuacInstruction original;
    original.opcode = "blob";
    original.args   = {"1", "abc123=="};

    std::string wire = GuacParser::serialize(original);

    GuacParser p;
    p.feed(wire);
    ASSERT_TRUE(p.has_instruction());
    auto restored = p.next_instruction();
    EXPECT_EQ(restored.opcode,    original.opcode);
    EXPECT_EQ(restored.args,      original.args);
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacParser reset()
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 12: reset() — 파싱 중간 상태 + 완성 큐 모두 초기화 ──────────────
//
// 파싱 도중 reset()을 호출하면:
//   - 이미 큐에 들어간 완성 명령어가 사라져야 한다.
//   - 중간에 읽던 데이터가 버려져 새 명령어를 처음부터 파싱할 수 있어야 한다.
TEST(GuacParserReset, ResetClearsStateAndQueue) {
    GuacParser p;

    // 완성 명령어 하나 + 미완성 절반 공급
    p.feed("3.img;3.en");
    ASSERT_TRUE(p.has_instruction());  // "img"는 큐에 있어야 함

    p.reset();

    // reset 후 큐는 비어야 한다
    EXPECT_FALSE(p.has_instruction());

    // reset 후 새 명령어를 정상 파싱할 수 있어야 한다
    p.feed("3.end;");
    ASSERT_TRUE(p.has_instruction());
    EXPECT_EQ(p.next_instruction().opcode, "end");
}
