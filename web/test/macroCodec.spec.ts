import {
  compactKeyTapSteps,
  decodeRuntimeMacro,
  encodeRuntimeMacro,
  type RuntimeMacroStep,
} from "../src/macroCodec";

describe("runtime macro codec", () => {
  it("encodes explicit key sequence steps as packed key opcode", () => {
    const encoded = encodeRuntimeMacro([
      { action: "keySequence", packedKeys: [0x04, 0x84] },
    ]);

    expect([...encoded]).toEqual([1, 5, 2, 0x04, 0x84]);
    expect(decodeRuntimeMacro(encoded)).toEqual([
      { action: "keySequence", packedKeys: [0x04, 0x84] },
    ]);
  });

  it("compacts consecutive key press taps into a key sequence step", () => {
    const steps: RuntimeMacroStep[] = [
      { action: "tap", behaviorId: 9, param1: 0x00070004, param2: 0 },
      { action: "tap", behaviorId: 9, param1: 0x02070004, param2: 0 },
      { action: "delay", delayMs: 10 },
    ];

    expect(compactKeyTapSteps(steps, { keyPressBehaviorId: 9 })).toEqual([
      { action: "keySequence", packedKeys: [0x04, 0x84] },
      { action: "delay", delayMs: 10 },
    ]);
    expect([...encodeRuntimeMacro(steps, { keyPressBehaviorId: 9 })]).toEqual([
      1, 5, 2, 0x04, 0x84, 4, 10,
    ]);
  });

  it("does not compact taps from other behaviors", () => {
    const steps: RuntimeMacroStep[] = [
      { action: "tap", behaviorId: 10, param1: 0x00070004, param2: 0 },
    ];

    expect(compactKeyTapSteps(steps, { keyPressBehaviorId: 9 })).toEqual(steps);
  });

  it("splits a packed key run longer than 64 keys across multiple keySequence steps", () => {
    // Matches the firmware proto's KeyTapSequenceStep.packed_keys max_size:64
    // (see proto/cormoran/runtime_macro/runtime_macro.options) - a single
    // wire step cannot carry more than 64 packed keys even though a macro's
    // overall byte budget (CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES) now allows
    // bodies well beyond what used to fit in one packed run.
    const steps: RuntimeMacroStep[] = Array.from({ length: 130 }, () => ({
      action: "tap" as const,
      behaviorId: 9,
      param1: 0x00070004,
      param2: 0,
    }));

    const compacted = compactKeyTapSteps(steps, { keyPressBehaviorId: 9 });

    expect(compacted).toHaveLength(3);
    expect(compacted[0]).toEqual({
      action: "keySequence",
      packedKeys: Array(64).fill(0x04),
    });
    expect(compacted[1]).toEqual({
      action: "keySequence",
      packedKeys: Array(64).fill(0x04),
    });
    expect(compacted[2]).toEqual({
      action: "keySequence",
      packedKeys: Array(2).fill(0x04),
    });
    for (const step of compacted) {
      if (step.action === "keySequence") {
        expect(step.packedKeys.length).toBeLessThanOrEqual(64);
      }
    }

    // Round-trips through encode/decode without exceeding the per-step cap.
    const encoded = encodeRuntimeMacro(steps, { keyPressBehaviorId: 9 });
    const decoded = decodeRuntimeMacro(encoded);
    expect(decoded).toEqual(compacted);
  });

  it("handles macros with more than 32 steps (no client-side step cap)", () => {
    const steps: RuntimeMacroStep[] = [];
    for (let i = 0; i < 40; i++) {
      steps.push({ action: "delay", delayMs: i + 1 });
      steps.push({
        action: "tap",
        behaviorId: 3,
        param1: i,
        param2: 0,
      });
    }
    // 80 non-mergeable steps - comfortably above the old MacroSlot.steps
    // max_count:32 and within the new max_count:64 per RPC round trip when
    // batched, but here we only exercise the wire-format codec (which has no
    // step-count ceiling of its own; RUNTIME_MACRO_RPC_MAX_STEPS/max_count
    // bound the Studio RPC transport, not this encode/decode pair).
    const encoded = encodeRuntimeMacro(steps);
    const decoded = decodeRuntimeMacro(encoded);

    expect(decoded).toHaveLength(steps.length);
    expect(decoded).toEqual(steps);
  });
});
