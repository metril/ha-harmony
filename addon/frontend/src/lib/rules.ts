// Every limit and rule of the config (harmony_core) and how the editor deals with it BEFORE apply.
// Keyed by the rule names in tests/py/test_editor_rules.py; test/rules.test.ts checks that every
// backend rule has a row here, that `inline` rows resolve to a visible place, and counts the gaps.
//   prevented: the control cannot produce the invalid value (generated, clamped, disabled, not offered)
//   inline:    the user can build it; the draft preview returns the server's message, shown at the field
//   gap:       only a banner / meter tells them (or nothing); listed in the report
export type Handling = 'prevented' | 'inline' | 'gap';
export interface RuleUi {
  how: Handling;
  by: string;
}
const P = (by: string): RuleUi => ({ how: 'prevented', by });
const I = (by: string): RuleUi => ({ how: 'inline', by });

export const RULES: Record<string, RuleUi> = {
  'device id pattern': P('ids are generated (deviceIdFor, 24 chars + suffix)'),
  'device name 31 bytes': P('input trimmed by UTF-8 bytes (trimBytes) + n/31 B counter'),
  'command name 31 bytes': P('nameProblem blocks Save with a byte count'),
  'reserved command power_on': P('nameProblem'),
  'reserved command input_x': P('nameProblem'),
  'power needs both or toggle': P('power mode control writes both or toggle'),
  'settle_ms 0..60000': P('number input clamped'),
  'port mask 1..255': P('port toggles write 1..7'),
  'repeats 1..8': P('repeat control offers 1..8'),
  'IR form exactly one': P('code dialog builds exactly one form'),
  'alias is another device id': I('Problems list on the device page (aliases only come from imports)'),
  'duplicate device id': P('ids are unique-suffixed'),
  'max 16 devices': P('Add device refuses at 16'),
  'max 256 commands': P('Add command, Duplicate and the wizard refuse past 256 (capProblem) and say why; same for 64 inputs'),
  'HA device takes only ha actions': P('HA device commands use the Home Assistant action picker'),
  'HA device power needs on and off': I('Problems list on the device page'),
  'HA device entity pattern': I('Problems list on the device page'),
  'activity id `off` reserved': P('activityId renames off to off_activity'),
  'activity unknown device': P('chain only offers existing devices; deleting a device clears references'),
  'activity unknown input': P('input picker lists the device inputs; deleting clears references'),
  'step unknown command': P('steps come from the picker; deleting clears references'),
  'step may not start an activity': P('the step picker has no activity kind'),
  'max steps per activity': P('Add step disabled at 16'),
  'key name': P('keys are the fixed remote keys'),
  'key action unknown device': P('picker lists existing devices'),
  'bt usage 0..65535': P('no Bluetooth action in the UI'),
  'ha service domain allow-list': P('service list filtered to the allow-list; typed services checked by haProblem'),
  'ha max 8 entities': P('the picker takes one entity'),
  'ha entity id pattern': P('haProblem blocks Set action'),
  'ha no templates': P('haProblem blocks Set action'),
  'ha data depth 4': P('haProblem blocks Set action'),
  'ha data 1024 bytes': P('haProblem blocks Set action'),
  'ha repeat is boolean': P('checkbox'),
  'page id pattern': P('page ids are generated'),
  'page title 24 bytes': P('title input trimmed by bytes (trimToBytes) + n/24 B counter'),
  'page cols 1..16': P('number input clamped'),
  'duplicate page id': P('uniquePageId'),
  'unknown activity on a page': P('select of existing activities; deleting an activity clears it'),
  'max 32 widgets per page': P('drops refused with widgetCap'),
  'max 32 pages': P('Add / Duplicate page refuse with pageCap'),
  'max 128 soft keys': P('drops refused with widgetCap (soft-key total)'),
  'widget label 24 bytes': P('label input stops at the page budget'),
  'page 251 bytes': I('page byte meter and the compiler message in the page inspector; thumbnail turns red'),
  'widget position 0..15': P('grid snapping'),
  'widget id pattern': P('widget ids are generated'),
  'duplicate widget id': P('uniqueWidgetId'),
  'button needs action': P('new buttons start with an explicit "no action"'),
  'tile needs entity': P('new tiles start with an entity'),
  'tile entity pattern': I('widget inspector shows the server message'),
  'tap toggle needs a toggleable entity': P('Toggle disabled for entities that cannot toggle'),
  'unknown icon': P('icon grid'),
  'role values': P('swatches'),
  'shape values': P('swatches'),
  'nav next on the last page': P('option disabled on the last page'),
  'nav home needs a home page': P('option disabled without a Home page'),
  'nav to unknown page': P('select of existing pages; deleting a page clears references'),
  'one home page': P('Kind control refuses a second Home page'),
  'list page has no x/y': P('list pages never get x/y'),
  'list rows 1..16': P('number input clamped'),
  'editor size': P('the store prunes editor metadata to ids the editor can show (boundEditor), far under the cap'),
};

export type Surface = 'key' | 'widget' | 'page' | 'device' | 'activity' | 'banner';
/** Which screen shows an error with this path (the studio, a device page, an activity page, or the shell banner). */
export function surfaceOf(path: (string | number)[]): Surface {
  const [a, b, c] = path;
  if (a === 'keys' && typeof b === 'string') return 'key';
  if (a === 'activities' && typeof b === 'number') return c === 'keys' ? 'key' : 'activity';
  if (a === 'devices' && typeof b === 'number') return 'device';
  if (a === 'pages' && typeof b === 'number') return c === 'widgets' ? 'widget' : 'page';
  return 'banner';
}
