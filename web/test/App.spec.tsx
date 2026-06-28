import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { setupZMKMocks } from "@cormoran/zmk-studio-react-hook/testing";
import App from "../src/App";

// Mock the ZMK client
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

jest.mock("@zmkfirmware/zmk-studio-ts-client/transport/serial", () => ({
  connect: jest.fn(),
}));

describe("App Component", () => {
  describe("Basic Rendering", () => {
    it("should render the application header", () => {
      render(<App />);

      expect(screen.getByText(/Runtime Macro/i)).toBeInTheDocument();
      expect(
        screen.getByText(/runtime-configurable ZMK macros/i)
      ).toBeInTheDocument();
    });

    it("should render connection button when disconnected", () => {
      render(<App />);

      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();
    });
  });

  describe("Connection Flow", () => {
    let mocks: ReturnType<typeof setupZMKMocks>;

    beforeEach(() => {
      mocks = setupZMKMocks();
    });

    it("should connect to device when connect button is clicked", async () => {
      mocks.mockSuccessfulConnection({
        deviceName: "Test Keyboard",
        subsystems: ["cormoran__runtime_macro"],
      });

      const { connect: serial_connect } =
        await import("@zmkfirmware/zmk-studio-ts-client/transport/serial");
      (serial_connect as jest.Mock).mockResolvedValue(mocks.mockTransport);

      render(<App />);

      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();

      const user = userEvent.setup();
      const connectButton = screen.getByText(/Connect Serial/i);
      await user.click(connectButton);

      await waitFor(() => {
        expect(screen.getByText(/Test Keyboard/i)).toBeInTheDocument();
      });

      expect(screen.getByText(/Disconnect/i)).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: "Macros" })
      ).toBeInTheDocument();
    });
  });
});
