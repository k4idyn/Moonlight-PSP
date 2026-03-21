import json

path = r'C:\Users\beelink\Applications\Files\Apollo\config\sunshine_state.json'
with open(path, 'r', encoding='utf-8') as f:
    state = json.load(f)

original_count = len(state['root']['named_devices'])
targets = ['test', 'moonlight-psp', 'PSPMoonlight']
# Standard Forensic Repair: Also remove 16-char hex names to ensure fresh pairing
new_devices = []
for d in state['root']['named_devices']:
    name = d['name']
    is_hex_id = len(name) == 16 and all(c in '0123456789ABCDEFabcdef' for c in name)
    if name not in targets and not is_hex_id:
        new_devices.append(d)

state['root']['named_devices'] = new_devices
new_count = len(state['root']['named_devices'])

with open(path, 'w', encoding='utf-8') as f:
    json.dump(state, f, indent=4)

print(f"Removed {original_count - new_count} device(s) (targets or hex IDs).")
