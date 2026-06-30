export type RuntimeMacroAction =
  | "down"
  | "up"
  | "tap"
  | "delay"
  | "keySequence";

export type RuntimeMacroStep =
  | {
      action: "down" | "up" | "tap";
      behaviorId: number;
      param1: number;
      param2: number;
    }
  | {
      action: "delay";
      delayMs: number;
    }
  | {
      action: "keySequence";
      packedKeys: number[];
    };

export const FORMAT_VERSION = 1;

export type RuntimeMacroCodecOptions = {
  keyPressBehaviorId?: number;
};

const OPCODES: Record<RuntimeMacroAction, number> = {
  down: 1,
  up: 2,
  tap: 3,
  delay: 4,
  keySequence: 5,
};

const HID_USAGE_KEY = 0x07;
const MOD_LSFT = 0x02;
const PACKED_LSFT = 0x80;
const PACKED_USAGE_MASK = 0x7f;
const PACKED_MIN_USAGE = 0x04;
const PACKED_MAX_USAGE = 0x38;

function assertUint32(value: number, label: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new Error(`${label} must be an unsigned 32-bit integer`);
  }
  return value;
}

function writeUvar(bytes: number[], value: number, label: string) {
  let remaining = assertUint32(value, label);
  do {
    let byte = remaining & 0x7f;
    remaining >>>= 7;
    if (remaining !== 0) byte |= 0x80;
    bytes.push(byte);
  } while (remaining !== 0);
}

function readUvar(bytes: Uint8Array, offset: { value: number }): number {
  let result = 0;
  let shift = 0;

  while (offset.value < bytes.length && shift <= 28) {
    const byte = bytes[offset.value++];
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) return result >>> 0;
    shift += 7;
  }

  throw new Error("Invalid varuint in encoded macro");
}

function packKeyTap(keycode: number): number | null {
  const uintKeycode = assertUint32(keycode, "keycode");
  const mods = uintKeycode >>> 24;
  if ((mods & ~MOD_LSFT) !== 0) return null;

  const usageKeycode = uintKeycode & ~(0xff << 24);
  const page = (usageKeycode >>> 16) & 0xff;
  const usage = usageKeycode & 0xffff;
  if (
    page !== HID_USAGE_KEY ||
    usage < PACKED_MIN_USAGE ||
    usage > PACKED_MAX_USAGE
  ) {
    return null;
  }

  return usage | (mods === MOD_LSFT ? PACKED_LSFT : 0);
}

function unpackKeyTap(packedKey: number): number {
  if (!Number.isInteger(packedKey) || packedKey < 0 || packedKey > 0xff) {
    throw new Error("packed key must be an unsigned byte");
  }

  const usage = packedKey & PACKED_USAGE_MASK;
  if (usage < PACKED_MIN_USAGE || usage > PACKED_MAX_USAGE) {
    throw new Error(`Unsupported packed key usage: ${usage}`);
  }

  const keycode = (HID_USAGE_KEY << 16) | usage;
  return packedKey & PACKED_LSFT ? ((MOD_LSFT << 24) | keycode) >>> 0 : keycode;
}

function writeKeySequence(bytes: number[], packedKeys: number[]) {
  bytes.push(OPCODES.keySequence);
  writeUvar(bytes, packedKeys.length, "packed key count");
  for (const packedKey of packedKeys) {
    unpackKeyTap(packedKey);
    bytes.push(packedKey);
  }
}

function readKeySequence(
  bytes: Uint8Array,
  offset: { value: number }
): number[] {
  const length = readUvar(bytes, offset);
  if (offset.value + length > bytes.length) {
    throw new Error("Invalid packed key sequence length in encoded macro");
  }

  const packedKeys: number[] = [];
  for (let i = 0; i < length; i++) {
    const packedKey = bytes[offset.value++];
    unpackKeyTap(packedKey);
    packedKeys.push(packedKey);
  }
  return packedKeys;
}

function packedTapKey(
  step: RuntimeMacroStep,
  options: RuntimeMacroCodecOptions
): number | null {
  if (
    step.action !== "tap" ||
    step.param2 !== 0 ||
    step.behaviorId !== options.keyPressBehaviorId
  ) {
    return null;
  }

  return packKeyTap(step.param1);
}

export function compactKeyTapSteps(
  steps: RuntimeMacroStep[],
  options: RuntimeMacroCodecOptions = {}
): RuntimeMacroStep[] {
  const compacted: RuntimeMacroStep[] = [];
  let packedBuffer: number[] = [];

  const flushPacked = () => {
    if (packedBuffer.length > 0) {
      compacted.push({ action: "keySequence", packedKeys: packedBuffer });
      packedBuffer = [];
    }
  };

  for (const step of steps) {
    if (step.action === "keySequence") {
      for (const packedKey of step.packedKeys) unpackKeyTap(packedKey);
      packedBuffer.push(...step.packedKeys);
      continue;
    }

    const packedKey = packedTapKey(step, options);
    if (packedKey !== null) {
      packedBuffer.push(packedKey);
      continue;
    }

    flushPacked();
    compacted.push(step);
  }

  flushPacked();
  return compacted;
}

export function encodeRuntimeMacro(
  steps: RuntimeMacroStep[],
  options: RuntimeMacroCodecOptions = {}
): Uint8Array {
  const bytes = [FORMAT_VERSION];

  for (const step of compactKeyTapSteps(steps, options)) {
    if (step.action === "keySequence") {
      writeKeySequence(bytes, step.packedKeys);
      continue;
    }

    bytes.push(OPCODES[step.action]);

    if (step.action === "delay") {
      writeUvar(bytes, step.delayMs, "delayMs");
      continue;
    }

    writeUvar(bytes, step.behaviorId, "behaviorId");
    writeUvar(bytes, step.param1, "param1");
    writeUvar(bytes, step.param2, "param2");
  }

  return Uint8Array.from(bytes);
}

export function decodeRuntimeMacro(encoded: Uint8Array): RuntimeMacroStep[] {
  if (encoded.length === 0) return [];
  if (encoded[0] !== FORMAT_VERSION) {
    throw new Error(`Unsupported macro format version: ${encoded[0]}`);
  }

  const offset = { value: 1 };
  const steps: RuntimeMacroStep[] = [];

  while (offset.value < encoded.length) {
    const opcode = encoded[offset.value++];

    if (opcode === OPCODES.keySequence) {
      steps.push({
        action: "keySequence",
        packedKeys: readKeySequence(encoded, offset),
      });
      continue;
    }

    if (opcode === OPCODES.delay) {
      steps.push({ action: "delay", delayMs: readUvar(encoded, offset) });
      continue;
    }

    const behaviorId = readUvar(encoded, offset);
    const param1 = readUvar(encoded, offset);
    const param2 = readUvar(encoded, offset);

    if (opcode === OPCODES.down) {
      steps.push({ action: "down", behaviorId, param1, param2 });
    } else if (opcode === OPCODES.up) {
      steps.push({ action: "up", behaviorId, param1, param2 });
    } else if (opcode === OPCODES.tap) {
      steps.push({ action: "tap", behaviorId, param1, param2 });
    } else {
      throw new Error(`Unknown macro opcode: ${opcode}`);
    }
  }

  return steps;
}

type AbyssMacroStep =
  | {
      action: "down" | "up";
      binding: { type: "raw"; zmk: string; label: string };
    }
  | {
      action: "tap";
      binding: { type: "raw"; zmk: string; label: string };
      tapTime?: number;
    }
  | {
      action: "delay";
      delayTime: number;
    };

function bindingToRawZmk(
  step: Exclude<RuntimeMacroStep, { action: "delay" | "keySequence" }>
) {
  return `local-id:${step.behaviorId} ${step.param1} ${step.param2}`;
}

function parseRawZmkBinding(binding: unknown) {
  if (!binding || typeof binding !== "object") {
    throw new Error("Macro binding must be an object");
  }

  const raw = binding as { type?: unknown; zmk?: unknown };
  if (raw.type !== "raw" || typeof raw.zmk !== "string") {
    throw new Error(
      "Only raw zmk bindings exported by this UI can be imported"
    );
  }

  const match = raw.zmk.trim().match(/^local-id:(\d+)\s+(\d+)\s+(\d+)$/);
  if (!match) {
    throw new Error(`Unsupported raw zmk binding: ${raw.zmk}`);
  }

  return {
    behaviorId: Number(match[1]),
    param1: Number(match[2]),
    param2: Number(match[3]),
  };
}

export function toKeyboardAbyssSteps(
  steps: RuntimeMacroStep[],
  options: RuntimeMacroCodecOptions = {}
): AbyssMacroStep[] {
  return steps.flatMap((step): AbyssMacroStep[] => {
    if (step.action === "delay") {
      return [{ action: "delay", delayTime: step.delayMs }];
    }

    if (step.action === "keySequence") {
      return step.packedKeys.map((packedKey) => ({
        action: "tap",
        binding: {
          type: "raw",
          zmk: `local-id:${options.keyPressBehaviorId ?? 0} ${unpackKeyTap(
            packedKey
          )} 0`,
          label: `Packed key 0x${packedKey.toString(16).padStart(2, "0")}`,
        },
      }));
    }

    const binding = {
      type: "raw" as const,
      zmk: bindingToRawZmk(step),
      label: `Behavior ${step.behaviorId}`,
    };

    if (step.action === "tap") {
      return [{ action: "tap", binding }];
    }

    return [{ action: step.action, binding }];
  });
}

export function fromKeyboardAbyssSteps(input: unknown): RuntimeMacroStep[] {
  if (!Array.isArray(input)) {
    throw new Error(
      "Keyboard Abyss macro JSON must be an array of macro steps"
    );
  }

  return input.map((value) => {
    if (!value || typeof value !== "object") {
      throw new Error("Macro step must be an object");
    }

    const step = value as {
      action?: unknown;
      binding?: unknown;
      delayTime?: unknown;
      tapTime?: unknown;
    };

    if (step.action === "delay") {
      return {
        action: "delay",
        delayMs: assertUint32(Number(step.delayTime), "delayTime"),
      };
    }

    if (
      step.action === "down" ||
      step.action === "up" ||
      step.action === "tap"
    ) {
      const binding = parseRawZmkBinding(step.binding);
      if (step.action === "tap") {
        if (step.tapTime !== undefined) {
          throw new Error(
            "tapTime is global in this firmware; use down-delay-up for per-step time"
          );
        }
        return {
          action: "tap",
          ...binding,
        };
      }
      return { action: step.action, ...binding };
    }

    throw new Error(`Unsupported macro action: ${String(step.action)}`);
  });
}
