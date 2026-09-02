// Browser-safe contract validator used by UI transport stubs. No command rules live here.
(function (target) {
  "use strict";

  target.createProtocolStubValidator = function createProtocolStubValidator(contract) {
    var commands = new Map(contract.commands.map(function (command) {
      return [command.kind, command.payload];
    }));

    function resolve(schema) {
      if (!schema.$ref) return schema;
      var prefix = "#/definitions/";
      if (schema.$ref.indexOf(prefix) !== 0) {
        throw new Error("contract contains unsupported ref: " + schema.$ref);
      }
      var resolved = contract.definitions[schema.$ref.slice(prefix.length)];
      if (!resolved) throw new Error("contract contains unknown ref: " + schema.$ref);
      return resolve(resolved);
    }

    function matches(value, schema, path) {
      try {
        validateValue(value, schema, path);
        return true;
      } catch (_) {
        return false;
      }
    }

    function validateValue(value, inputSchema, path) {
      var schema = resolve(inputSchema);
      if (schema.oneOf) {
        var count = schema.oneOf.filter(function (candidate) {
          return matches(value, candidate, path);
        }).length;
        if (count !== 1) throw new Error(path + " must match exactly one allowed shape");
        return;
      }
      if (Object.prototype.hasOwnProperty.call(schema, "const") && value !== schema.const) {
        throw new Error(path + " does not match the required constant");
      }
      if (schema.enum && schema.enum.indexOf(value) < 0) {
        throw new Error(path + " is not an allowed value");
      }
      if (!schema.type) return;

      if (schema.type === "object") {
        if (value === null || Array.isArray(value) || typeof value !== "object") {
          throw new Error(path + " must be object");
        }
        if (schema.maxProperties !== undefined && Object.keys(value).length > schema.maxProperties) {
          throw new Error(path + " has too many properties");
        }
        (schema.required || []).forEach(function (key) {
          if (!Object.prototype.hasOwnProperty.call(value, key)) {
            throw new Error(path + "." + key + " is required");
          }
        });
        Object.entries(schema.properties || {}).forEach(function (entry) {
          var key = entry[0];
          if (Object.prototype.hasOwnProperty.call(value, key)) {
            validateValue(value[key], entry[1], path + "." + key);
          }
        });
        return;
      }
      if (schema.type === "array") {
        if (!Array.isArray(value)) throw new Error(path + " must be array");
        if (schema.items) {
          value.forEach(function (item, index) {
            validateValue(item, schema.items, path + "[" + index + "]");
          });
        }
        return;
      }

      var valid =
        (schema.type === "string" && typeof value === "string") ||
        (schema.type === "boolean" && typeof value === "boolean") ||
        (schema.type === "null" && value === null) ||
        (schema.type === "number" && typeof value === "number" && Number.isFinite(value)) ||
        (schema.type === "integer" && Number.isSafeInteger(value));
      if (!valid) throw new Error(path + " must be " + schema.type);
      if (typeof value === "number") {
        if (schema.minimum !== undefined && value < schema.minimum) {
          throw new Error(path + " is below minimum");
        }
        if (schema.maximum !== undefined && value > schema.maximum) {
          throw new Error(path + " is above maximum");
        }
      }
    }

    return function validateCommand(kind, payload) {
      var schema = commands.get(kind);
      if (!schema) throw new Error("unknown command kind: " + kind);
      validateValue(payload, schema, "payload");
    };
  };
})(globalThis);
