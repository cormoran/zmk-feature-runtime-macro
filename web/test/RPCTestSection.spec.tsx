import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";

describe("RPCTestSection Component", () => {
  describe("With Subsystem", () => {
    it("should render RPC controls when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/RPC Test/i)).toBeInTheDocument();
      expect(screen.getByText(/Send a sample request/i)).toBeInTheDocument();
      expect(screen.getByLabelText(/Value:/i)).toBeInTheDocument();
      expect(screen.getByText(/Send Request/i)).toBeInTheDocument();
    });

    it("should show default input value", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      const input = screen.getByLabelText(/Value:/i) as HTMLInputElement;
      expect(input.value).toBe("42");
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
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "your_name__template" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the template module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<RPCTestSection />);

      expect(container.firstChild).toBeNull();
    });
  });
});
