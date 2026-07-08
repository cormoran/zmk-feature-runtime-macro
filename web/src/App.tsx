import { useCallback, useContext, useEffect, useState } from "react";
import "./App.css";
import { connect as serialConnect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
} from "./proto/cormoran/runtime_macro/runtime_macro";
import {
  Request as CustomSettingsRequest,
  Response as CustomSettingsResponse,
  SettingWriteMode,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";
import {
  compactKeyTapSteps,
  encodeRuntimeMacro,
  fromKeyboardAbyssSteps,
  toKeyboardAbyssSteps,
} from "./macroCodec";
import type { RuntimeMacroStep } from "./macroCodec";
import type { MacroStep as RpcMacroStep } from "./proto/cormoran/runtime_macro/runtime_macro";

export const SUBSYSTEM_IDENTIFIER = "cormoran__runtime_macro";
// The generic custom-settings Studio RPC subsystem - used only for
// CreateSetting/DeleteSetting (raw entry create/delete/rename), since those
// are already covered by zmk-feature-custom-settings and this module does
// not duplicate them in its own proto. See docs/design/keyspace-macros.md.
const CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER = "cormoran_custom_settings";
const MACRO_KEY_PREFIX = "macro/";

type MacroSummary = {
  slot: number;
  name: string;
  encodedSize: number;
};

type LoadedMacro = {
  slot: number;
  name: string;
  steps: RuntimeMacroStep[];
};

const DEFAULT_STEP: RuntimeMacroStep = {
  action: "tap",
  behaviorId: 0,
  param1: 0,
  param2: 0,
};

function runtimeStepToRpc(step: RuntimeMacroStep): RpcMacroStep {
  if (step.action === "delay") {
    return { delay: { delayMs: step.delayMs } };
  }
  if (step.action === "keySequence") {
    return { keyTapSequence: { packedKeys: Uint8Array.from(step.packedKeys) } };
  }

  const binding = {
    behaviorId: step.behaviorId,
    param1: step.param1,
    param2: step.param2,
  };

  if (step.action === "down") return { down: binding };
  if (step.action === "up") return { up: binding };
  return { tap: binding };
}

function runtimeStepFromRpc(step: RpcMacroStep): RuntimeMacroStep {
  if (step.down) return { action: "down", ...step.down };
  if (step.up) return { action: "up", ...step.up };
  if (step.tap) return { action: "tap", ...step.tap };
  if (step.delay) return { action: "delay", delayMs: step.delay.delayMs };
  if (step.keyTapSequence) {
    return {
      action: "keySequence",
      packedKeys: [...step.keyTapSequence.packedKeys],
    };
  }

  throw new Error("Macro step is missing oneof data");
}

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>Runtime Macro</h1>
        <p>
          Edit compact, runtime-configurable ZMK macros over custom Studio RPC.
        </p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="panel">
            <h2>Device</h2>
            {isLoading && <p>Connecting...</p>}
            {error && <p className="message error">{error}</p>}
            {!isLoading && (
              <button
                className="btn primary"
                onClick={() => connect(serialConnect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="panel device-row">
              <div>
                <h2>Device</h2>
                <p>{deviceName}</p>
              </div>
              <button className="btn" onClick={disconnect}>
                Disconnect
              </button>
            </section>
            <RuntimeMacroEditor />
          </>
        )}
      />
    </div>
  );
}

export function RuntimeMacroEditor() {
  const zmkApp = useContext(ZMKAppContext);
  const [macros, setMacros] = useState<MacroSummary[]>([]);
  const [selectedName, setSelectedName] = useState<string | null>(null);
  const [loadedMacro, setLoadedMacro] = useState<LoadedMacro | null>(null);
  const [maxMacroBytes, setMaxMacroBytes] = useState(64);
  const [maxNameLength, setMaxNameLength] = useState(32);
  const [poolBytesTotal, setPoolBytesTotal] = useState(0);
  const [poolBytesUsed, setPoolBytesUsed] = useState(0);
  const [tapMs, setTapMs] = useState(30);
  const [keyPressBehaviorId, setKeyPressBehaviorId] = useState<
    number | undefined
  >(undefined);
  const [jsonText, setJsonText] = useState("[]");
  const [newMacroName, setNewMacroName] = useState("");
  const [renameTo, setRenameTo] = useState("");
  const [message, setMessage] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);
  const customSettingsSubsystem = zmkApp?.findSubsystem(
    CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER
  );
  const connection = zmkApp?.state.connection;
  const subsystemIndex = subsystem?.index;
  const customSettingsSubsystemIndex = customSettingsSubsystem?.index;
  const serviceReady = connection && subsystemIndex !== undefined;

  const callRPC = useCallback(
    async (request: Request) => {
      if (!connection || subsystemIndex === undefined) {
        throw new Error("Runtime macro subsystem is not connected");
      }
      const service = new ZMKCustomSubsystem(connection, subsystemIndex);
      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!responsePayload) throw new Error("Empty RPC response");
      const response = Response.decode(responsePayload);
      if (response.error) throw new Error(response.error.message);
      return response;
    },
    [connection, subsystemIndex]
  );

  const callCustomSettingsRPC = useCallback(
    async (request: CustomSettingsRequest) => {
      if (!connection || customSettingsSubsystemIndex === undefined) {
        throw new Error("custom-settings subsystem is not connected");
      }
      const service = new ZMKCustomSubsystem(
        connection,
        customSettingsSubsystemIndex
      );
      const payload = CustomSettingsRequest.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!responsePayload) throw new Error("Empty RPC response");
      const response = CustomSettingsResponse.decode(responsePayload);
      if (response.error) throw new Error(response.error.message);
      return response;
    },
    [connection, customSettingsSubsystemIndex]
  );

  const loadMacro = useCallback(
    async (name: string, keyPressId = keyPressBehaviorId) => {
      setIsLoading(true);
      setMessage(null);
      try {
        const response = await callRPC(Request.create({ getMacro: { name } }));
        const macro = response.getMacro?.macro;
        if (!macro) throw new Error("Macro was missing from RPC response");

        const steps = macro.steps.map(runtimeStepFromRpc);
        setSelectedName(name);
        setLoadedMacro({ slot: macro.slot, name: macro.name, steps });
        setRenameTo(macro.name);
        setJsonText(
          JSON.stringify(
            toKeyboardAbyssSteps(steps, { keyPressBehaviorId: keyPressId }),
            null,
            2
          )
        );
      } catch (error) {
        setMessage(
          error instanceof Error ? error.message : "Failed to load macro"
        );
      } finally {
        setIsLoading(false);
      }
    },
    [callRPC, keyPressBehaviorId]
  );

  const refreshList = useCallback(async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const response = await callRPC(Request.create({ listMacros: {} }));
      const list = response.listMacros?.macros ?? [];
      setMacros(
        list.map((macro) => ({
          slot: macro.slot,
          name: macro.name,
          encodedSize: macro.encodedSize,
        }))
      );
      setMaxMacroBytes(response.listMacros?.maxMacroBytes || 64);
      setMaxNameLength(response.listMacros?.maxNameLength || 32);
      const globalSettings = await callRPC(
        Request.create({ getMacroGlobalSettings: {} })
      );
      const settings = globalSettings.getMacroGlobalSettings?.settings;
      const nextKeyPressBehaviorId = settings?.keyPressBehaviorId || undefined;
      setTapMs(settings?.tapMs ?? 30);
      setKeyPressBehaviorId(nextKeyPressBehaviorId);
      setPoolBytesTotal(settings?.poolBytesTotal ?? 0);
      setPoolBytesUsed(settings?.poolBytesUsed ?? 0);
      if (list.length > 0) {
        const stillSelected = list.find((m) => m.name === selectedName);
        await loadMacro(
          (stillSelected ?? list[0]).name,
          nextKeyPressBehaviorId
        );
      } else {
        setSelectedName(null);
        setLoadedMacro(null);
      }
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to list macros"
      );
    } finally {
      setIsLoading(false);
    }
  }, [callRPC, loadMacro, selectedName]);

  useEffect(() => {
    if (!serviceReady) return;
    const timer = window.setTimeout(() => void refreshList(), 0);
    return () => window.clearTimeout(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [serviceReady]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="panel">
        <p className="message warning">
          Subsystem "{SUBSYSTEM_IDENTIFIER}" was not found. Build firmware with
          runtime macro RPC enabled.
        </p>
      </section>
    );
  }

  const updateStep = (stepIndex: number, nextStep: RuntimeMacroStep) => {
    if (!loadedMacro) return;
    setLoadedMacro({
      ...loadedMacro,
      steps: loadedMacro.steps.map((step, index) =>
        index === stepIndex ? nextStep : step
      ),
    });
  };

  const removeStep = (stepIndex: number) => {
    if (!loadedMacro) return;
    const steps = loadedMacro.steps.filter((_, index) => index !== stepIndex);
    setLoadedMacro({ ...loadedMacro, steps });
    setJsonText(
      JSON.stringify(
        toKeyboardAbyssSteps(steps, { keyPressBehaviorId }),
        null,
        2
      )
    );
  };

  const addStep = () => {
    if (!loadedMacro) return;
    setLoadedMacro({
      ...loadedMacro,
      steps: [...loadedMacro.steps, DEFAULT_STEP],
    });
  };

  const saveMacro = async (persist: boolean) => {
    if (!loadedMacro) return;
    setIsLoading(true);
    setMessage(null);

    try {
      const codecOptions = { keyPressBehaviorId };
      const rpcSteps = compactKeyTapSteps(loadedMacro.steps, codecOptions);
      const encodedMacro = encodeRuntimeMacro(rpcSteps, codecOptions);
      if (encodedMacro.length > maxMacroBytes) {
        throw new Error(
          `Encoded macro is ${encodedMacro.length} bytes; limit is ${maxMacroBytes}`
        );
      }

      await callRPC(
        Request.create({
          setMacroStepCount: {
            name: loadedMacro.name,
            stepCount: rpcSteps.length,
            persist,
          },
        })
      );
      for (const [stepIndex, step] of rpcSteps.entries()) {
        await callRPC(
          Request.create({
            setMacroStep: {
              name: loadedMacro.name,
              stepIndex,
              step: runtimeStepToRpc(step),
              persist,
            },
          })
        );
      }

      setJsonText(
        JSON.stringify(toKeyboardAbyssSteps(rpcSteps, codecOptions), null, 2)
      );
      setMessage(
        persist ? "Saved to persistent settings" : "Updated in memory"
      );
      await refreshList();
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to save macro"
      );
    } finally {
      setIsLoading(false);
    }
  };

  const saveTapMs = async (persist: boolean) => {
    setIsLoading(true);
    setMessage(null);

    try {
      await callRPC(
        Request.create({
          setTapMs: {
            tapMs,
            persist,
          },
        })
      );
      setMessage(
        persist
          ? "Saved tap_ms to persistent settings"
          : "Updated tap_ms in memory"
      );
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to save tap_ms"
      );
    } finally {
      setIsLoading(false);
    }
  };

  // Raw create/delete/rename go through the generic custom-settings
  // CreateSetting/DeleteSetting RPC (subsystem "cormoran_custom_settings"),
  // not a runtime-macro-specific request - see docs/design/keyspace-macros.md.
  const createMacro = async () => {
    const name = newMacroName.trim();
    if (!name) return;
    if (customSettingsSubsystemIndex === undefined) {
      setMessage(
        `Subsystem "${CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER}" was not found - build firmware with custom-settings RPC enabled`
      );
      return;
    }

    setIsLoading(true);
    setMessage(null);
    try {
      await callCustomSettingsRPC(
        CustomSettingsRequest.create({
          createSetting: {
            setting: { key: MACRO_KEY_PREFIX + name },
            value: { bytesValue: Uint8Array.from([1]) }, // format version, no steps
            mode: SettingWriteMode.SETTING_WRITE_MODE_MEMORY,
          },
        })
      );
      setNewMacroName("");
      setMessage(`Created "${name}"`);
      setSelectedName(name);
      await refreshList();
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to create macro"
      );
    } finally {
      setIsLoading(false);
    }
  };

  const deleteMacro = async () => {
    if (!loadedMacro) return;
    if (customSettingsSubsystemIndex === undefined) {
      setMessage(
        `Subsystem "${CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER}" was not found - build firmware with custom-settings RPC enabled`
      );
      return;
    }

    setIsLoading(true);
    setMessage(null);
    try {
      await callCustomSettingsRPC(
        CustomSettingsRequest.create({
          deleteSetting: {
            setting: { key: MACRO_KEY_PREFIX + loadedMacro.name },
          },
        })
      );
      setLoadedMacro(null);
      setSelectedName(null);
      setJsonText("[]");
      setMessage(`Deleted "${loadedMacro.name}"`);
      await refreshList();
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to delete macro"
      );
    } finally {
      setIsLoading(false);
    }
  };

  const renameMacro = async () => {
    if (!loadedMacro) return;
    const newName = renameTo.trim();
    if (!newName || newName === loadedMacro.name) return;
    if (customSettingsSubsystemIndex === undefined) {
      setMessage(
        `Subsystem "${CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER}" was not found - build firmware with custom-settings RPC enabled`
      );
      return;
    }

    setIsLoading(true);
    setMessage(null);
    try {
      const encodedMacro = encodeRuntimeMacro(
        compactKeyTapSteps(loadedMacro.steps, { keyPressBehaviorId }),
        { keyPressBehaviorId }
      );
      // Create the new name first (carrying the current body), then delete
      // the old one - so a failure never loses data (see
      // zmk_runtime_macro_rename's doc comment).
      await callCustomSettingsRPC(
        CustomSettingsRequest.create({
          createSetting: {
            setting: { key: MACRO_KEY_PREFIX + newName },
            value: { bytesValue: encodedMacro },
            mode: SettingWriteMode.SETTING_WRITE_MODE_MEMORY,
          },
        })
      );
      await callCustomSettingsRPC(
        CustomSettingsRequest.create({
          deleteSetting: {
            setting: { key: MACRO_KEY_PREFIX + loadedMacro.name },
          },
        })
      );
      setMessage(`Renamed to "${newName}"`);
      setSelectedName(newName);
      await refreshList();
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to rename macro"
      );
    } finally {
      setIsLoading(false);
    }
  };

  const applyPendingMacros = async (action: "save" | "discard") => {
    setIsLoading(true);
    setMessage(null);

    try {
      const response = await callRPC(
        Request.create(
          action === "save" ? { saveMacros: {} } : { discardMacros: {} }
        )
      );
      await refreshList();
      setMessage(
        response.status?.message ??
          (action === "save"
            ? "Saved pending macro changes"
            : "Discarded pending macro changes")
      );
    } catch (error) {
      setMessage(
        error instanceof Error
          ? error.message
          : `Failed to ${action} pending macro changes`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const importJson = () => {
    if (!loadedMacro) return;
    try {
      const codecOptions = { keyPressBehaviorId };
      const steps = compactKeyTapSteps(
        fromKeyboardAbyssSteps(JSON.parse(jsonText)),
        codecOptions
      );
      const encoded = encodeRuntimeMacro(steps, codecOptions);
      if (encoded.length > maxMacroBytes) {
        throw new Error(
          `Imported macro is ${encoded.length} bytes; limit is ${maxMacroBytes}`
        );
      }
      setLoadedMacro({ ...loadedMacro, steps });
      setMessage("Imported Keyboard Abyss macro steps");
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to import JSON"
      );
    }
  };

  const encodedSize = loadedMacro
    ? encodeRuntimeMacro(loadedMacro.steps, { keyPressBehaviorId }).length
    : 0;

  return (
    <main className="workspace">
      <aside className="macro-list">
        <div className="section-title">
          <h2>Macros</h2>
          <button className="btn" onClick={refreshList} disabled={isLoading}>
            Refresh
          </button>
        </div>
        {poolBytesTotal > 0 && (
          <p
            className={
              poolBytesUsed >= poolBytesTotal
                ? "message warning pool-usage"
                : "message pool-usage"
            }
          >
            Shared macro pool: {poolBytesUsed}/{poolBytesTotal} B used
          </p>
        )}
        {macros.map((macro) => (
          <button
            key={macro.name}
            className={
              macro.name === selectedName ? "macro-row selected" : "macro-row"
            }
            onClick={() => loadMacro(macro.name)}
          >
            <span>{macro.name || `(unnamed slot ${macro.slot})`}</span>
            <small>
              slot {macro.slot} · {macro.encodedSize} B
            </small>
          </button>
        ))}
        <div className="create-macro">
          <input
            value={newMacroName}
            maxLength={maxNameLength}
            placeholder="New macro name"
            onChange={(event) => setNewMacroName(event.target.value)}
          />
          <button
            className="btn"
            onClick={createMacro}
            disabled={isLoading || !newMacroName.trim()}
          >
            Create
          </button>
        </div>
      </aside>

      <section className="editor">
        {loadedMacro ? (
          <>
            <div className="global-settings">
              <label>
                Tap ms
                <input
                  type="number"
                  min={0}
                  max={10000}
                  value={tapMs}
                  onChange={(event) =>
                    setTapMs(numericValue(event.target.value))
                  }
                />
              </label>
              <button
                className="btn"
                onClick={() => saveTapMs(false)}
                disabled={isLoading}
              >
                Write Memory
              </button>
              <button
                className="btn"
                onClick={() => saveTapMs(true)}
                disabled={isLoading}
              >
                Save
              </button>
            </div>

            <div className="editor-head">
              <label>
                Name (slot {loadedMacro.slot})
                <input
                  value={renameTo}
                  maxLength={maxNameLength}
                  onChange={(event) => setRenameTo(event.target.value)}
                />
              </label>
              <button
                className="btn"
                onClick={renameMacro}
                disabled={
                  isLoading || !renameTo.trim() || renameTo === loadedMacro.name
                }
              >
                Rename
              </button>
              <div
                className={
                  encodedSize > maxMacroBytes ? "byte-count over" : "byte-count"
                }
              >
                {encodedSize}/{maxMacroBytes} B
              </div>
            </div>

            <div className="steps">
              {loadedMacro.steps.map((step, index) => (
                <StepEditor
                  key={index}
                  step={step}
                  onChange={(nextStep) => updateStep(index, nextStep)}
                  onRemove={() => removeStep(index)}
                />
              ))}
            </div>

            <div className="actions">
              <button className="btn" onClick={addStep}>
                Add Step
              </button>
              <button
                className="btn"
                onClick={() => saveMacro(false)}
                disabled={isLoading}
              >
                Write Memory
              </button>
              <button
                className="btn primary"
                onClick={() => saveMacro(true)}
                disabled={isLoading}
              >
                Save
              </button>
              <button
                className="btn"
                onClick={() => applyPendingMacros("save")}
                disabled={isLoading}
              >
                Save Pending
              </button>
              <button
                className="btn"
                onClick={() => applyPendingMacros("discard")}
                disabled={isLoading}
              >
                Discard Pending
              </button>
              <button
                className="btn danger"
                onClick={deleteMacro}
                disabled={isLoading}
              >
                Delete
              </button>
            </div>

            <div className="json-pane">
              <div className="section-title">
                <h2>Keyboard Abyss Steps</h2>
                <button className="btn" onClick={importJson}>
                  Import
                </button>
              </div>
              <textarea
                value={jsonText}
                onChange={(event) => setJsonText(event.target.value)}
                spellCheck={false}
              />
            </div>
          </>
        ) : (
          <p>No macro selected. Create one to get started.</p>
        )}

        {message && <p className="message">{message}</p>}
      </section>
    </main>
  );
}

function numericValue(value: string) {
  return Number.isFinite(Number(value)) ? Math.max(0, Number(value)) : 0;
}

function formatPackedKeys(packedKeys: number[]) {
  return packedKeys
    .map((value) => value.toString(16).padStart(2, "0"))
    .join(" ");
}

function parsePackedKeys(value: string) {
  if (value.trim() === "") return [];
  return value
    .split(/[\s,]+/)
    .filter(Boolean)
    .map((part) => {
      const parsed = Number.parseInt(part.replace(/^0x/i, ""), 16);
      return Number.isFinite(parsed) ? Math.max(0, Math.min(255, parsed)) : 0;
    });
}

function StepEditor({
  step,
  onChange,
  onRemove,
}: {
  step: RuntimeMacroStep;
  onChange: (step: RuntimeMacroStep) => void;
  onRemove: () => void;
}) {
  const setAction = (action: RuntimeMacroStep["action"]) => {
    if (action === "delay") {
      onChange({ action: "delay", delayMs: 10 });
    } else if (action === "keySequence") {
      onChange({ action: "keySequence", packedKeys: [] });
    } else if (action === "tap") {
      onChange({
        action: "tap",
        behaviorId: 0,
        param1: 0,
        param2: 0,
      });
    } else {
      onChange({ action, behaviorId: 0, param1: 0, param2: 0 });
    }
  };

  return (
    <div className="step-row">
      <select
        value={step.action}
        onChange={(event) => setAction(event.target.value as never)}
      >
        <option value="tap">Tap</option>
        <option value="keySequence">Key Seq</option>
        <option value="down">Down</option>
        <option value="up">Up</option>
        <option value="delay">Delay</option>
      </select>

      {step.action === "delay" ? (
        <label>
          Delay ms
          <input
            type="number"
            min={0}
            value={step.delayMs}
            onChange={(event) =>
              onChange({
                action: "delay",
                delayMs: numericValue(event.target.value),
              })
            }
          />
        </label>
      ) : step.action === "keySequence" ? (
        <label>
          Packed keys
          <input
            value={formatPackedKeys(step.packedKeys)}
            onChange={(event) =>
              onChange({
                action: "keySequence",
                packedKeys: parsePackedKeys(event.target.value),
              })
            }
          />
        </label>
      ) : (
        <>
          <label>
            Behavior ID
            <input
              type="number"
              min={0}
              value={step.behaviorId}
              onChange={(event) =>
                onChange({
                  ...step,
                  behaviorId: numericValue(event.target.value),
                })
              }
            />
          </label>
          <label>
            Param 1
            <input
              type="number"
              min={0}
              value={step.param1}
              onChange={(event) =>
                onChange({ ...step, param1: numericValue(event.target.value) })
              }
            />
          </label>
          <label>
            Param 2
            <input
              type="number"
              min={0}
              value={step.param2}
              onChange={(event) =>
                onChange({ ...step, param2: numericValue(event.target.value) })
              }
            />
          </label>
        </>
      )}

      <button className="btn danger" onClick={onRemove}>
        Remove
      </button>
    </div>
  );
}

export default App;
