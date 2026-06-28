export type RuntimeMacroAction = "down" | "up" | "tap" | "delay";

export type RuntimeMacroStep =
  | {
      action: "down" | "up";
      behaviorId: number;
      param1: number;
      param2: number;
    }
  | {
      action: "tap";
      behaviorId: number;
      param1: number;
      param2: number;
    }
  | {
      action: "delay";
      delayMs: number;
    };

export const FORMAT_VERSION = 1;

const OPCODES: Record<RuntimeMacroAction, number> = {
  down: 1,
  up: 2,
  tap: 3,
  delay: 4,
};

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

export function encodeRuntimeMacro(steps: RuntimeMacroStep[]): Uint8Array {
  const bytes = [FORMAT_VERSION];

  for (const step of steps) {
    bytes.push(OPCODES[step.action]);

    if (step.action === "delay") {
      writeUvar(bytes, step.delayMs, "delayMs");
      continue;
    }

    if (step.action === "tap") {
      writeUvar(bytes, step.behaviorId, "behaviorId");
      writeUvar(bytes, step.param1, "param1");
      writeUvar(bytes, step.param2, "param2");
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

function bindingToRawZmk(step: Exclude<RuntimeMacroStep, { action: "delay" }>) {
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
  steps: RuntimeMacroStep[]
): AbyssMacroStep[] {
  return steps.map((step) => {
    if (step.action === "delay") {
      return { action: "delay", delayTime: step.delayMs };
    }

    const binding = {
      type: "raw" as const,
      zmk: bindingToRawZmk(step),
      label: `Behavior ${step.behaviorId}`,
    };

    if (step.action === "tap") {
      return { action: "tap", binding };
    }

    return { action: step.action, binding };
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
