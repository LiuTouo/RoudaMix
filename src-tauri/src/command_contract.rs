use serde_json::Value;
use std::sync::OnceLock;

const CONTRACT_JSON: &str = include_str!("../../contracts/command_contract.json");

fn table() -> &'static Value {
    static TABLE: OnceLock<Value> = OnceLock::new();
    TABLE.get_or_init(|| {
        serde_json::from_str(CONTRACT_JSON).expect("embedded command contract must be valid JSON")
    })
}

fn resolve_ref<'a>(root: &'a Value, reference: &str) -> Result<&'a Value, String> {
    let name = reference
        .strip_prefix("#/definitions/")
        .ok_or_else(|| format!("contract contains unsupported ref: {reference}"))?;
    root.get("definitions")
        .and_then(|definitions| definitions.get(name))
        .ok_or_else(|| format!("contract contains unknown ref: {reference}"))
}

fn matches(value: &Value, schema: &Value, root: &Value, path: &str) -> bool {
    validate(value, schema, root, path).is_ok()
}

fn validate(value: &Value, schema: &Value, root: &Value, path: &str) -> Result<(), String> {
    if let Some(reference) = schema.get("$ref").and_then(Value::as_str) {
        return validate(value, resolve_ref(root, reference)?, root, path);
    }
    if let Some(candidates) = schema.get("oneOf").and_then(Value::as_array) {
        let count = candidates
            .iter()
            .filter(|candidate| matches(value, candidate, root, path))
            .count();
        return if count == 1 {
            Ok(())
        } else {
            Err(format!("{path} must match exactly one allowed shape"))
        };
    }
    if let Some(expected) = schema.get("const") {
        if value != expected {
            return Err(format!("{path} does not match the required constant"));
        }
    }
    if let Some(allowed) = schema.get("enum").and_then(Value::as_array) {
        if !allowed.contains(value) {
            return Err(format!("{path} is not an allowed value"));
        }
    }

    let Some(kind) = schema.get("type").and_then(Value::as_str) else {
        return Ok(());
    };
    match kind {
        "object" => {
            let object = value
                .as_object()
                .ok_or_else(|| format!("{path} must be object"))?;
            if let Some(maximum) = schema.get("maxProperties").and_then(Value::as_u64) {
                if object.len() as u64 > maximum {
                    return Err(format!("{path} has too many properties"));
                }
            }
            if let Some(required) = schema.get("required").and_then(Value::as_array) {
                for key in required.iter().filter_map(Value::as_str) {
                    if !object.contains_key(key) {
                        return Err(format!("{path}.{key} is required"));
                    }
                }
            }
            if let Some(properties) = schema.get("properties").and_then(Value::as_object) {
                for (key, property_schema) in properties {
                    if let Some(property) = object.get(key) {
                        validate(property, property_schema, root, &format!("{path}.{key}"))?;
                    }
                }
            }
        }
        "array" => {
            let array = value
                .as_array()
                .ok_or_else(|| format!("{path} must be array"))?;
            if let Some(item_schema) = schema.get("items") {
                for (index, item) in array.iter().enumerate() {
                    validate(item, item_schema, root, &format!("{path}[{index}]"))?;
                }
            }
        }
        "string" if !value.is_string() => return Err(format!("{path} must be string")),
        "boolean" if !value.is_boolean() => return Err(format!("{path} must be boolean")),
        "null" if !value.is_null() => return Err(format!("{path} must be null")),
        "number" if !value.is_number() => return Err(format!("{path} must be number")),
        "integer" if value.as_u64().is_none() && value.as_i64().is_none() => {
            return Err(format!("{path} must be integer"));
        }
        _ => {}
    }

    if value.is_number() {
        let number = value
            .as_f64()
            .ok_or_else(|| format!("{path} must be a finite number"))?;
        if schema
            .get("minimum")
            .and_then(Value::as_f64)
            .is_some_and(|minimum| number < minimum)
        {
            return Err(format!("{path} is below minimum"));
        }
        if schema
            .get("maximum")
            .and_then(Value::as_f64)
            .is_some_and(|maximum| number > maximum)
        {
            return Err(format!("{path} is above maximum"));
        }
    }
    Ok(())
}

fn find_kind(collection: &str, kind: &str) -> Option<&'static Value> {
    table()
        .get(collection)?
        .as_array()?
        .iter()
        .find(|entry| entry.get("kind").and_then(Value::as_str) == Some(kind))
}

pub fn is_error_code(code: &str) -> bool {
    table()["errorCodes"]
        .as_array()
        .is_some_and(|codes| codes.iter().any(|entry| entry["code"] == code))
}

pub fn error_codes() -> Vec<&'static str> {
    table()["errorCodes"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|entry| entry["code"].as_str())
        .collect()
}

pub fn is_event_kind(kind: &str) -> bool {
    find_kind("events", kind).is_some()
}

pub fn validate_command_payload(kind: &str, payload: &Value) -> Result<(), String> {
    let command =
        find_kind("commands", kind).ok_or_else(|| format!("unknown command kind: {kind}"))?;
    validate(payload, &command["payload"], table(), "payload")
}
