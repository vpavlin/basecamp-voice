R"SCHEMA({
  "type": "object",
  "properties": {
    "reply": {"type": "string"},
    "steps": {
      "type": "array",
      "maxItems": 5,
      "items": {
        "anyOf": [
          {"type": "object", "properties": {"tool": {"const": "open_app"}, "args": {"type": "object", "properties": {"app": {"type": "string"}}, "required": ["app"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "install"}, "args": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "list_installed"}, "args": {"type": "object", "properties": {}, "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "list_available"}, "args": {"type": "object", "properties": {"query": {"type": "string"}, "repository": {"type": "string"}}, "required": ["query"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "add_repository"}, "args": {"type": "object", "properties": {"url": {"type": "string"}}, "required": ["url"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "status"}, "args": {"type": "object", "properties": {}, "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "module_methods"}, "args": {"type": "object", "properties": {"module": {"type": "string"}}, "required": ["module"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "recipe"}, "args": {"type": "object", "properties": {"app": {"type": "string"}, "action": {"type": "string"}}, "required": ["app", "action"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "intent"}, "args": {"type": "object", "properties": {"intent": {"type": "string"}, "params": {"type": "object"}}, "required": ["intent", "params"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false},
          {"type": "object", "properties": {"tool": {"const": "call"}, "args": {"type": "object", "properties": {"module": {"type": "string"}, "method": {"type": "string"}, "args": {"type": "array", "maxItems": 4}}, "required": ["module", "method", "args"], "additionalProperties": false}}, "required": ["tool", "args"], "additionalProperties": false}
        ]
      }
    }
  },
  "required": ["reply", "steps"],
  "additionalProperties": false
})SCHEMA"
