// One list of what is wrong with the config: server errors, add-on warnings, Studio checks.
import type { Config, ValidationError } from './model';
import { summarizeErrors } from './errors';
import type { StudioCheck } from './studio-model';

export type IssueLevel = 'error' | 'warning';
export interface Issue {
  level: IssueLevel;
  text: string;
  target?: { kind: 'route'; route: string } | { kind: 'widget'; page: number; index?: number };
  fix?: { label: string; value: string }; // label overflow only: value = the short label
}

export function issueText(c: StudioCheck): string {
  return c.msg;
}

const fromServer = (list: ValidationError[], level: IssueLevel, config: Config | null): Issue[] =>
  summarizeErrors(list, config).located.map((l) => ({ level, text: l.msg, target: { kind: 'route', route: l.route } }));

export function collectIssues(errors: ValidationError[], warnings: ValidationError[], checks: StudioCheck[], config: Config | null): Issue[] {
  const out = [...fromServer(errors, 'error', config), ...fromServer(warnings, 'warning', config)];
  for (const c of checks) {
    const issue: Issue = { level: 'warning', text: issueText(c) };
    if (c.page !== undefined) issue.target = { kind: 'widget', page: c.page, ...(c.index !== undefined ? { index: c.index } : {}) };
    if (c.kind === 'label' && c.short) issue.fix = { label: `Use "${c.short}"`, value: c.short };
    out.push(issue);
  }
  return out;
}

/** First issue per widget index on a page; errors win over warnings. */
export function widgetIssues(issues: Issue[], page: number): Map<number, Issue> {
  const m = new Map<number, Issue>();
  for (const i of issues) {
    const t = i.target;
    if (t?.kind !== 'widget' || t.page !== page || t.index === undefined) continue;
    const cur = m.get(t.index);
    if (!cur || (cur.level === 'warning' && i.level === 'error')) m.set(t.index, i);
  }
  return m;
}
