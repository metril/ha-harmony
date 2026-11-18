import js from '@eslint/js';
import tseslint from 'typescript-eslint';

// CSP-safe rendering (spec section 4): no inline style attributes, no innerHTML / unsafeHTML.
export default tseslint.config(
  { ignores: ['dist', 'node_modules'] },
  js.configs.recommended,
  ...tseslint.configs.strict,
  { files: ['test/**'], rules: { '@typescript-eslint/no-non-null-assertion': 'off' } },
  {
    files: ['src/**'],
    rules: {
      '@typescript-eslint/no-dynamic-delete': 'off',
      'no-restricted-syntax': [
        'error',
        {
          // `style=${setStyle(..)}` (unquoted) is the one allowed form; quoted or static style attributes are not.
          selector: `TaggedTemplateExpression[tag.name="html"] TemplateElement[value.raw=/\\sstyle\\s*=\\s*["']/]`,
          message: 'No style="..." attributes: use Lit static styles or styleMap (CSP).',
        },
        {
          selector: 'MemberExpression[property.name=/^(innerHTML|outerHTML)$/]',
          message: 'No innerHTML/outerHTML (CSP-safe rendering rule).',
        },
        {
          selector: 'CallExpression[callee.property.name="insertAdjacentHTML"]',
          message: 'No insertAdjacentHTML.',
        },
      ],
      'no-restricted-imports': [
        'error',
        {
          patterns: [
            { group: ['**/unsafe-html*', '**/unsafe-svg*'], message: 'No unsafeHTML/unsafeSVG.' },
            { group: ['**/style-map*'], message: 'styleMap sets a style attribute on first render, which the CSP blocks. Use setStyle.' },
          ],
        },
      ],
    },
  },
);
