// The cheap, local part of the Home Assistant action rules (harmony_core/ha_actions.py). The server
// stays the authority (its message is shown when it disagrees); these catch typos while typing.
export const ACTION_DOMAINS = ['light', 'switch', 'scene', 'script', 'media_player', 'remote', 'fan', 'cover', 'climate', 'input_boolean', 'input_button', 'button', 'number', 'select'];
export const SERVICE_RE = /^[a-z_]+\.[a-z0-9_]+$/;
export const ENTITY_RE = /^[a-z0-9_]+\.[a-z0-9_]+$/;
export const TEMPLATE_MARKS = ['{{', '{%', '{#'];
export const DATA_MAX_BYTES = 1024;
export const DATA_MAX_STR = 200;
export const DATA_MAX_DEPTH = 4;
export const DATA_MAX_NODES = 64;
const TARGET_KEYS = ['entity_id', 'device_id', 'area_id', 'floor_id', 'label_id'];

const hasTemplate = (s: string) => TEMPLATE_MARKS.some((m) => s.includes(m));

function walk(v: unknown, depth = 1, count = { n: 0 }): string | null {
  if (++count.n > DATA_MAX_NODES) return `data has more than ${DATA_MAX_NODES} values`;
  if (depth > DATA_MAX_DEPTH) return `data is nested deeper than ${DATA_MAX_DEPTH}`;
  if (typeof v === 'string') {
    if (v.length > DATA_MAX_STR) return `a data string is over ${DATA_MAX_STR} characters`;
    if (hasTemplate(v)) return 'data must not contain a template ({{ }}, {% %} or {# #})';
  } else if (Array.isArray(v)) {
    for (const x of v) {
      const r = walk(x, depth + 1, count);
      if (r) return r;
    }
  } else if (v && typeof v === 'object') {
    for (const [k, x] of Object.entries(v)) {
      if (hasTemplate(k)) return 'a data key must not contain a template ({{ }}, {% %} or {# #})';
      if (TARGET_KEYS.includes(k)) return `put '${k}' in the target, not in the data`;
      const r = walk(x, depth + 1, count);
      if (r) return r;
    }
  }
  return null;
}

/** Why this service/entity/data cannot be an action, or null. Same wording as the server where it has one. */
export function haProblem(service: string, entity: string, data?: Record<string, unknown>): string | null {
  if (service) {
    if (!SERVICE_RE.test(service)) return 'service must look like `domain.service` (a-z, 0-9, _)';
    if (hasTemplate(service)) return 'service must not contain a template ({{ }}, {% %} or {# #})';
    const d = service.split('.', 1)[0] as string;
    if (!ACTION_DOMAINS.includes(d)) return `service domain '${d}' is not allowed; use one of ${[...ACTION_DOMAINS].sort().join(', ')}`;
  }
  if (entity && !ENTITY_RE.test(entity)) return `'${entity}' is not an entity id`;
  if (data) {
    const w = walk(data);
    if (w) return w;
    if (new TextEncoder().encode(JSON.stringify(data)).length > DATA_MAX_BYTES) return `data is over ${DATA_MAX_BYTES} bytes`;
  }
  return null;
}
