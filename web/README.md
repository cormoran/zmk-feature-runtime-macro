# Runtime Macro Web UI

React + TypeScript UI for editing runtime macros over the custom Studio RPC subsystem `cormoran__runtime_macro`.

```bash
npm ci
npm run generate
npm test
npm run build
```

The protobuf schema is defined in `../proto/cormoran/runtime_macro/runtime_macro.proto`.

The UI edits macro names and compact encoded macro bodies. It can import/export the supported Keyboard Abyss macro step subset: `down`, `up`, `tap`, and `delay`.
