import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { RuntimeMacroEditor, SUBSYSTEM_IDENTIFIER } from "../src/App";

describe("RuntimeMacroEditor Component", () => {
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
