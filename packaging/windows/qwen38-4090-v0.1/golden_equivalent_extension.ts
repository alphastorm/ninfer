import { readFileSync } from "node:fs";
import type { ExtensionAPI } from "@oh-my-pi/pi-coding-agent";

interface GoldenContract {
  tool: {
    name: string;
    label: string;
    description: string;
    expected_arguments: { city: string; days: number; metric: boolean };
    result: string;
  };
  final_visible_answer: string;
}

function parseContract(value: unknown): GoldenContract {
  if (typeof value !== "object" || value === null || !("tool" in value) || !("final_visible_answer" in value)) {
    throw new Error("Golden-equivalent contract shape mismatch");
  }
  const tool = value.tool;
  if (
    typeof tool !== "object" ||
    tool === null ||
    !("name" in tool) ||
    typeof tool.name !== "string" ||
    !("label" in tool) ||
    typeof tool.label !== "string" ||
    !("description" in tool) ||
    typeof tool.description !== "string" ||
    !("expected_arguments" in tool) ||
    typeof tool.expected_arguments !== "object" ||
    tool.expected_arguments === null ||
    !("city" in tool.expected_arguments) ||
    typeof tool.expected_arguments.city !== "string" ||
    !("days" in tool.expected_arguments) ||
    typeof tool.expected_arguments.days !== "number" ||
    !("metric" in tool.expected_arguments) ||
    typeof tool.expected_arguments.metric !== "boolean" ||
    !("result" in tool) ||
    typeof tool.result !== "string" ||
    typeof value.final_visible_answer !== "string"
  ) {
    throw new Error("Golden-equivalent contract shape mismatch");
  }
  return {
    tool: {
      name: tool.name,
      label: tool.label,
      description: tool.description,
      expected_arguments: {
        city: tool.expected_arguments.city,
        days: tool.expected_arguments.days,
        metric: tool.expected_arguments.metric,
      },
      result: tool.result,
    },
    final_visible_answer: value.final_visible_answer,
  };
}

const contract = parseContract(
  JSON.parse(readFileSync(new URL("./golden_equivalent_contract.json", import.meta.url), "utf8")) as unknown,
);

export default function registerGoldenEquivalent(pi: ExtensionAPI) {
  const { z } = pi.zod;
  const expected = contract.tool.expected_arguments;

  pi.registerTool({
    name: contract.tool.name,
    label: contract.tool.label,
    description: contract.tool.description,
    loadMode: "essential",
    approval: "read",
    parameters: z
      .object({
        city: z.literal(expected.city).describe("The city string; it must be Paris."),
        days: z.literal(expected.days).describe("The integer forecast length; it must be 3."),
        metric: z.literal(expected.metric).describe("The metric-unit boolean; it must be true."),
      })
      .strict(),
    async execute(_toolCallId: string, params: unknown) {
      const value = params as Record<string, unknown>;
      if (
        Object.keys(value).length !== 3 ||
        typeof value.city !== "string" ||
        value.city !== expected.city ||
        typeof value.days !== "number" ||
        !Number.isInteger(value.days) ||
        value.days !== expected.days ||
        typeof value.metric !== "boolean" ||
        value.metric !== expected.metric
      ) {
        throw new Error("Golden-equivalent typed argument oracle rejected the invocation");
      }
      return {
        content: [{ type: "text", text: contract.tool.result }],
        details: {
          contract: "qwen38-4090-omp-golden-equivalent-v1",
          finalVisibleAnswer: contract.final_visible_answer,
          typedArgumentOraclePassed: true,
        },
      };
    },
  });
}
