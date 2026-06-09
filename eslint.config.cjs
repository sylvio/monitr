const js = require('@eslint/js');
const globals = require('globals');

module.exports = [
    js.configs.recommended,
    {
        files: ['examples/**/*.js', 'lib/**/*.js', 'tests/**/*.js'],
        languageOptions: {
            ecmaVersion: 2022,
            sourceType: 'commonjs',
            globals: {
                ...globals.node,
            },
        },
        rules: {
            indent: ['error', 4, {
                SwitchCase: 1,
            }],

            'linebreak-style': ['error', 'unix'],
            'no-console': 'off',
            quotes: ['error', 'single'],
            semi: ['error', 'always'],
        },
    },
];
