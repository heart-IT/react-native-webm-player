import reactNative from '@react-native/eslint-config/flat';

export default [
  ...reactNative,
  {
    // eslint-plugin-ft-flow 2.0.3 calls context.getAllComments, removed in
    // ESLint 9; the example has no Flow files.
    files: ['**/*.js'],
    rules: {
      'ft-flow/define-flow-type': 'off',
      'ft-flow/use-flow-type': 'off',
    },
  },
];
