// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny JSON writer shared by the MPO Ghidra scripts (no external dependencies,
// so the scripts compile with Ghidra's bundled script compiler).
//@category MPO

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public final class MpoJson {
    private MpoJson() {}

    /** Returns `value` as a JSON string literal. */
    public static String str(String value) {
        StringBuilder out = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            switch (c) {
                case '"' -> out.append("\\\"");
                case '\\' -> out.append("\\\\");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                default -> {
                    if (c < 0x20 || c > 0x7E) {
                        out.append(String.format("\\u%04x", (int) c));
                    } else {
                        out.append(c);
                    }
                }
            }
        }
        return out.append('"').toString();
    }

    /** Returns `value` as a quoted 0x-prefixed hex string ("0x08804000"). */
    public static String hex(long value) {
        return str(String.format("0x%08X", value));
    }

    /** Serializes nested Map/List/String/Number/Boolean/null values. */
    public static String value(Object value) {
        if (value == null) {
            return "null";
        }
        if (value instanceof String s) {
            return str(s);
        }
        if (value instanceof Number || value instanceof Boolean) {
            return value.toString();
        }
        if (value instanceof Raw raw) {
            return raw.json();
        }
        if (value instanceof Map<?, ?> map) {
            List<String> parts = new ArrayList<>();
            for (Map.Entry<?, ?> e : map.entrySet()) {
                parts.add(str(e.getKey().toString()) + ":" + value(e.getValue()));
            }
            return "{" + String.join(",", parts) + "}";
        }
        if (value instanceof Iterable<?> list) {
            List<String> parts = new ArrayList<>();
            for (Object item : list) {
                parts.add(value(item));
            }
            return "[" + String.join(",", parts) + "]";
        }
        throw new IllegalArgumentException("unsupported JSON value: " + value.getClass());
    }

    /** Pre-serialized JSON fragment. */
    public record Raw(String json) {}
}
