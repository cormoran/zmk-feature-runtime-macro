import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { RuntimeMacroEditor, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  Request,
  Response,
} from "../src/proto/cormoran/runtime_macro/runtime_macro";
import {
  Request as CustomSettingsRequest,
  Response as CustomSettingsResponse,
} from "../src/proto/cormoran/zmk/custom_settings/custom_settings";

const CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER = "cormoran_custom_settings";
// Matches createMockSubsystems' array-index-as-subsystem-index convention -
// see [SUBSYSTEM_IDENTIFIER, CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER] below.
const CUSTOM_SETTINGS_SUBSYSTEM_INDEX = 1;

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  call_rpc: jest.fn(),
}));

describe("RuntimeMacroEditor Component", () => {
  const rpcRequests: Request[] = [];
  const customSettingsRequests: CustomSettingsRequest[] = [];

  const mockRuntimeMacroRpc = () => {
    const { call_rpc } = jest.requireMock("@zmkfirmware/zmk-studio-ts-client");
    (call_rpc as jest.Mock).mockImplementation(
      async (
        _connection: unknown,
        rpcRequest: {
          custom?: { call?: { subsystemIndex?: number; payload?: Uint8Array } };
        }
      ) => {
        const payload = rpcRequest.custom?.call?.payload;
        if (!payload) {
          throw new Error("Missing custom RPC payload");
        }

        if (
          rpcRequest.custom?.call?.subsystemIndex ===
          CUSTOM_SETTINGS_SUBSYSTEM_INDEX
        ) {
          const request = CustomSettingsRequest.decode(payload);
          customSettingsRequests.push(request);
          const response = CustomSettingsResponse.create({
            status: { affectedCount: 1, message: "OK" },
          });
          return {
            custom: {
              call: {
                payload: CustomSettingsResponse.encode(response).finish(),
              },
            },
          };
        }

        const request = Request.decode(payload);
        rpcRequests.push(request);

        let response: Response;
        if (request.listMacros) {
          response = Response.create({
            listMacros: {
              macros: [{ slot: 0, name: "Test Macro", encodedSize: 1 }],
              maxMacroBytes: 64,
              maxNameLength: 32,
            },
          });
        } else if (request.getMacroGlobalSettings) {
          response = Response.create({
            getMacroGlobalSettings: {
              settings: { tapMs: 30, maxEntries: 8, keyPressBehaviorId: 1 },
            },
          });
        } else if (request.getMacro) {
          response = Response.create({
            getMacro: {
              macro: {
                slot: 0,
                name: request.getMacro.name,
                steps: [],
                encodedSize: 0,
              },
            },
          });
        } else if (request.saveMacros) {
          response = Response.create({
            status: {
              affectedCount: 3,
              message: "Runtime macro settings saved",
            },
          });
        } else if (request.discardMacros) {
          response = Response.create({
            status: {
              affectedCount: 3,
              message: "Runtime macro settings discarded",
            },
          });
        } else {
          response = Response.create({
            status: { affectedCount: 1, message: "OK" },
          });
        }

        return {
          custom: { call: { payload: Response.encode(response).finish() } },
        };
      }
    );
  };

  beforeEach(() => {
    jest.clearAllMocks();
    rpcRequests.length = 0;
    customSettingsRequests.length = 0;
    mockRuntimeMacroRpc();
  });

  describe("With Subsystem", () => {
    it("should render macro editor when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RuntimeMacroEditor />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: "Macros" })
      ).toBeInTheDocument();
      expect(screen.getByText(/Refresh/i)).toBeInTheDocument();
    });

    it("should send save and discard requests for pending macro changes", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RuntimeMacroEditor />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByDisplayValue("Test Macro")).toBeInTheDocument();
      });

      const user = userEvent.setup();
      await user.click(screen.getByRole("button", { name: "Save Pending" }));

      await waitFor(() => {
        expect(rpcRequests.some((request) => request.saveMacros)).toBe(true);
      });
      expect(
        screen.getByText("Runtime macro settings saved")
      ).toBeInTheDocument();

      await user.click(screen.getByRole("button", { name: "Discard Pending" }));

      await waitFor(() => {
        expect(rpcRequests.some((request) => request.discardMacros)).toBe(true);
      });
      expect(
        screen.getByText("Runtime macro settings discarded")
      ).toBeInTheDocument();
    });
  });

  describe("Create/Delete via generic custom-settings RPC", () => {
    it("creates a macro with CreateSetting on the custom-settings subsystem", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [
          SUBSYSTEM_IDENTIFIER,
          CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER,
        ],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RuntimeMacroEditor />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByDisplayValue("Test Macro")).toBeInTheDocument();
      });

      const user = userEvent.setup();
      await user.type(
        screen.getByPlaceholderText("New macro name"),
        "New Macro"
      );
      await user.click(screen.getByRole("button", { name: "Create" }));

      await waitFor(() => {
        expect(
          customSettingsRequests.some(
            (request) =>
              request.createSetting?.setting?.key === "macro/New Macro"
          )
        ).toBe(true);
      });
    });

    it("deletes the loaded macro with DeleteSetting on the custom-settings subsystem", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [
          SUBSYSTEM_IDENTIFIER,
          CUSTOM_SETTINGS_SUBSYSTEM_IDENTIFIER,
        ],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RuntimeMacroEditor />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByDisplayValue("Test Macro")).toBeInTheDocument();
      });

      const user = userEvent.setup();
      await user.click(screen.getByRole("button", { name: "Delete" }));

      await waitFor(() => {
        expect(
          customSettingsRequests.some(
            (request) =>
              request.deleteSetting?.setting?.key === "macro/Test Macro"
          )
        ).toBe(true);
      });
    });
  });

  describe("Large macro support", () => {
    it("respects a dynamic max_macro_bytes and shows shared pool usage", async () => {
      const { call_rpc } = jest.requireMock(
        "@zmkfirmware/zmk-studio-ts-client"
      );
      (call_rpc as jest.Mock).mockImplementation(
        async (
          _connection: unknown,
          rpcRequest: { custom?: { call?: { payload?: Uint8Array } } }
        ) => {
          const payload = rpcRequest.custom?.call?.payload;
          if (!payload) {
            throw new Error("Missing custom RPC payload");
          }

          const request = Request.decode(payload);
          rpcRequests.push(request);

          let response: Response;
          if (request.listMacros) {
            response = Response.create({
              listMacros: {
                macros: [{ slot: 0, name: "Long Macro", encodedSize: 5 }],
                // Larger than the old fixed 64-byte carrier - the UI must
                // pick this up instead of a hardcoded limit.
                maxMacroBytes: 256,
                maxNameLength: 32,
              },
            });
          } else if (request.getMacroGlobalSettings) {
            response = Response.create({
              getMacroGlobalSettings: {
                settings: {
                  tapMs: 30,
                  maxEntries: 8,
                  keyPressBehaviorId: 1,
                  poolBytesTotal: 1024,
                  poolBytesUsed: 100,
                },
              },
            });
          } else if (request.getMacro) {
            response = Response.create({
              getMacro: {
                macro: {
                  slot: 0,
                  name: "Long Macro",
                  steps: [],
                  encodedSize: 5,
                },
              },
            });
          } else {
            response = Response.create({
              status: { affectedCount: 1, message: "OK" },
            });
          }

          return {
            custom: { call: { payload: Response.encode(response).finish() } },
          };
        }
      );

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RuntimeMacroEditor />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByDisplayValue("Long Macro")).toBeInTheDocument();
      });

      // Byte-count readout follows the server-reported limit, not a
      // hardcoded 64.
      expect(screen.getByText(/\/256 B/)).toBeInTheDocument();
      // Shared pool occupancy is surfaced from MacroGlobalSettings.
      expect(
        screen.getByText(/Shared macro pool: 100\/1024 B used/)
      ).toBeInTheDocument();
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RuntimeMacroEditor />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__runtime_macro" was not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(/runtime macro RPC enabled/i)
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<RuntimeMacroEditor />);

      expect(container.firstChild).toBeNull();
    });
  });
});
