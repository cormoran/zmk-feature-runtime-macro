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
  encodeRuntimeMacro,
  fromKeyboardAbyssSteps,
  toKeyboardAbyssSteps,
} from "./macroCodec";
import type { RuntimeMacroStep } from "./macroCodec";
import type { MacroStep as RpcMacroStep } from "./proto/cormoran/runtime_macro/runtime_macro";

export const SUBSYSTEM_IDENTIFIER = "cormoran__runtime_macro";

type MacroSummary = {
  index: number;
  name: string;
  encodedSize: number;
};

type LoadedMacro = {
  index: number;
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
  const [selectedIndex, setSelectedIndex] = useState(0);
  const [loadedMacro, setLoadedMacro] = useState<LoadedMacro | null>(null);
  const [maxMacroBytes, setMaxMacroBytes] = useState(64);
  const [tapMs, setTapMs] = useState(30);
  const [jsonText, setJsonText] = useState("[]");
  const [message, setMessage] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);
  const connection = zmkApp?.state.connection;
  const subsystemIndex = subsystem?.index;
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

  const loadMacro = useCallback(
    async (index: number) => {
      setIsLoading(true);
      setMessage(null);
      try {
        const response = await callRPC(Request.create({ getMacro: { index } }));
        const macro = response.getMacro?.macro;
        if (!macro) throw new Error("Macro was missing from RPC response");

        const steps = macro.steps.map(runtimeStepFromRpc);
        setSelectedIndex(index);
        setLoadedMacro({ index, name: macro.name, steps });
        setJsonText(JSON.stringify(toKeyboardAbyssSteps(steps), null, 2));
      } catch (error) {
        setMessage(
          error instanceof Error ? error.message : "Failed to load macro"
        );
      } finally {
        setIsLoading(false);
      }
    },
    [callRPC]
  );

  const refreshList = useCallback(async () => {
    setIsLoading(true);
    setMessage(null);
    try {
      const response = await callRPC(Request.create({ listMacros: {} }));
      const list = response.listMacros?.macros ?? [];
      setMacros(
        list.map((macro) => ({
          index: macro.index,
          name: macro.name,
          encodedSize: macro.encodedSize,
        }))
      );
      setMaxMacroBytes(response.listMacros?.maxMacroBytes || 64);
      const globalSettings = await callRPC(
        Request.create({ getMacroGlobalSettings: {} })
      );
      setTapMs(globalSettings.getMacroGlobalSettings?.settings?.tapMs ?? 30);
      if (list.length > 0) {
        await loadMacro(list[Math.min(selectedIndex, list.length - 1)].index);
      }
    } catch (error) {
      setMessage(
        error instanceof Error ? error.message : "Failed to list macros"
      );
    } finally {
      setIsLoading(false);
    }
  }, [callRPC, loadMacro, selectedIndex]);

  useEffect(() => {
    if (!serviceReady) return;
    const timer = window.setTimeout(() => void refreshList(), 0);
    return () => window.clearTimeout(timer);
  }, [refreshList, serviceReady]);

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
    setJsonText(JSON.stringify(toKeyboardAbyssSteps(steps), null, 2));
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
      const encodedMacro = encodeRuntimeMacro(loadedMacro.steps);
      if (encodedMacro.length > maxMacroBytes) {
        throw new Error(
          `Encoded macro is ${encodedMacro.length} bytes; limit is ${maxMacroBytes}`
        );
      }

      await callRPC(
        Request.create({
          setMacroName: {
            index: loadedMacro.index,
            name: loadedMacro.name,
            persist,
          },
        })
      );
      await callRPC(
        Request.create({
          setMacroSteps: {
            index: loadedMacro.index,
            steps: loadedMacro.steps.map(runtimeStepToRpc),
            persist,
          },
        })
      );

      setJsonText(
        JSON.stringify(toKeyboardAbyssSteps(loadedMacro.steps), null, 2)
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

  const importJson = () => {
    if (!loadedMacro) return;
    try {
      const steps = fromKeyboardAbyssSteps(JSON.parse(jsonText));
      const encoded = encodeRuntimeMacro(steps);
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
    ? encodeRuntimeMacro(loadedMacro.steps).length
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
        {macros.map((macro) => (
          <button
            key={macro.index}
            className={
              macro.index === selectedIndex ? "macro-row selected" : "macro-row"
            }
            onClick={() => loadMacro(macro.index)}
          >
            <span>{macro.name || `Macro ${macro.index}`}</span>
            <small>{macro.encodedSize} B</small>
          </button>
        ))}
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
                Name
                <input
                  value={loadedMacro.name}
                  maxLength={64}
                  onChange={(event) =>
                    setLoadedMacro({ ...loadedMacro, name: event.target.value })
                  }
                />
              </label>
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
          <p>Select a macro slot.</p>
        )}

        {message && <p className="message">{message}</p>}
      </section>
    </main>
  );
}

function numericValue(value: string) {
  return Number.isFinite(Number(value)) ? Math.max(0, Number(value)) : 0;
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
