from pathlib import Path

HOME_DIR = Path(__file__).parent
DICT_PATH = HOME_DIR / 'ba1-dictionary.txt'

words = map(lambda t: t.upper(), set(DICT_PATH.read_text().splitlines()))
output = []
for word in words:
    bad = False
    if not 3 <= len(word) <= 16:
        bad = True
    elif 'Q' in word:
        for i in range(len(word)):
            if word[i] == 'Q' and word[i:i + 2] != 'QU':
                bad = True
                break
        if not bad:
            output.append(word.replace('QU', 'Q'))
    else:
        output.append(word)
    if bad:
        print(word, "bad")

# DICT_PATH.write_text('\n'.join(sorted(output)))

# words = set(DICT_PATH.read_text().splitlines())
# for file_path in HOME_DIR.iterdir():
#     if file_path != DICT_PATH:
#         data = set(file_path.read_text().splitlines())
#         new_data = data.intersection(words)
#         file_path.write_text('\n'.join(sorted(new_data)))
