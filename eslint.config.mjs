import js from '@eslint/js'
import tseslint from 'typescript-eslint'

export default tseslint.config(
  {
    ignores: ['lib/**', 'nitrogen/**', 'example/**', 'node_modules/**'],
  },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    rules: {
      // Nitro specs are type-only declarations; empty interfaces are the idiom
      // for prop bags that only inherit (e.g. HybridViewProps).
      '@typescript-eslint/no-empty-object-type': 'off',
    },
  }
)
