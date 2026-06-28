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

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  call_rpc: jest.fn(),
}));

describe("RuntimeMacroEditor Component", () => {
  const rpcRequests: Request[] = [];

  const mockRuntimeMacroRpc = () => {
    const { call_rpc } = jest.requireMock("@zmkfirmware/zmk-studio-ts-client");
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
              macros: [{ index: 0, name: "Test Macro", encodedSize: 1 }],
              maxMacroBytes: 64,
              maxNameLength: 64,
            },
          });
        } else if (request.getMacroGlobalSettings) {
          response = Response.create({
            getMacroGlobalSettings: { settings: { tapMs: 30, maxMacro: 8 } },
          });
        } else if (request.getMacro) {
          response = Response.create({
            getMacro: {
              macro: {
                index: request.getMacro.index,
                name: "Test Macro",
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
