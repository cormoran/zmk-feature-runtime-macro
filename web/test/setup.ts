// jest-dom adds custom jest matchers for asserting on DOM nodes.
import "@testing-library/jest-dom";
import { TextDecoder, TextEncoder } from "util";

Object.assign(globalThis, { TextDecoder, TextEncoder });
