#!/usr/bin/env python3
"""Check the remove-only public contract; editor behavior is tested separately over MCP."""

import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = ROOT / 'plugins/EngineWire/Source/EngineWire/Private/MCP/Extensions'


class RemoveUserParameterContract(unittest.TestCase):
    def test_destructive_unsaved_without_force_or_rename(self):
        records = [record for path in EXTENSIONS.glob('*.capabilities.json')
                   for record in json.loads(path.read_text())]
        matches = [record for record in records
                   if record['id'] == 'manage_effect.remove_niagara_user_parameter']
        self.assertEqual(len(matches), 1, 'Missing remove user parameter capability')
        record = matches[0]
        self.assertEqual(record['behavior']['effect'], 'destructive')
        self.assertEqual(record['policy']['consent'], 'explicit')
        schema = record['schemas']['input']
        self.assertFalse(schema['additionalProperties'])
        self.assertEqual(set(schema['properties']), {'action', 'systemPath', 'name'})
        self.assertEqual(set(schema['required']), {'action', 'systemPath', 'name'})
        self.assertIn('references', record['schemas']['output']['properties'])
        self.assertIn('undo', record['schemas']['output']['properties'])


if __name__ == '__main__':
    unittest.main()
