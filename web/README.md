# ZMK Module Template - Web Frontend

This is a minimal web application template for interacting with ZMK firmware
modules that implement custom Studio RPC subsystems.

## Features

- **Device Connection**: Connect to ZMK devices via Bluetooth (GATT) or Serial
- **Custom RPC**: Communicate with your custom firmware module using protobuf
- **React + TypeScript**: Modern web development with Vite for fast builds
- **react-zmk-studio**: Uses the `@cormoran/zmk-studio-react-hook` library for
  simplified ZMK integration

## Quick Start

```bash
# Install dependencies
npm install

# Generate TypeScript types from proto
npm run generate

# Run development server
npm run dev

# Build for production
npm run build

# Run tests
npm test
```

## Project Structure

```
src/
├── main.tsx              # React entry point
├── App.tsx               # Main application with connection UI
├── App.css               # Styles
└── proto/                # Generated protobuf TypeScript types
    └── your-name/template/
        └── template.ts

test/
├── App.spec.tsx              # Tests for App component
└── RPCTestSection.spec.tsx   # Tests for RPC functionality
```

## How It Works

### 1. Protocol Definition

The protobuf schema is defined in `../proto/your-name/template/template.proto`.

### 2. Code Generation

TypeScript types are generated using `ts-proto`:

```bash
npm run generate
```

This runs `buf generate` which uses the configuration in `buf.gen.yaml`.

### 3. Using react-zmk-studio

The app uses the `@cormoran/zmk-studio-react-hook` library:

```typescript
import { useZMKApp, ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";

// Connect to device
const { state, connect, findSubsystem, isConnected } = useZMKApp();

// Find your subsystem
const subsystem = findSubsystem("your_name__template");

// Create service and make RPC calls
const service = new ZMKCustomSubsystem(state.connection, subsystem.index);
const response = await service.callRPC(payload);
```

## Testing

```bash
# Run all tests
npm test

# Run tests in watch mode
npm run test:watch

# Run tests with coverage
npm run test:coverage
```

### Writing Tests

Use the test helpers from `@cormoran/zmk-studio-react-hook/testing`:

```typescript
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";

const mockZMKApp = createConnectedMockZMKApp({
  deviceName: "Test Device",
  subsystems: ["your_name__template"],
});

render(
  <ZMKAppProvider value={mockZMKApp}>
    <YourComponent />
  </ZMKAppProvider>
);
```

## Customization

To adapt this template for your own ZMK module:

1. **Update the proto file**: Modify `../proto/your-name/template/template.proto` with
   your message types
2. **Regenerate types**: Run `npm run generate`
3. **Update subsystem identifier**: Change `SUBSYSTEM_IDENTIFIER` in `App.tsx`
   to match your firmware registration
4. **Update RPC logic**: Modify the request/response handling in `App.tsx`
5. **Update tests**: Modify tests to match your custom subsystem identifier and
   functionality
