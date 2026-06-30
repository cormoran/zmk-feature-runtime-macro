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
});
