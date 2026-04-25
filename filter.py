import re

input_file = "english_dictionary.txt"
output_file = "english_dictionary_filtered.txt"

# Regex pattern: strictly A-Z, a-z, and hyphens (-)
pattern = re.compile(r'^[A-Za-z\-]+$')

kept_count = 0
removed_count = 0

print("Filtering dictionary...")

with open(input_file, 'r', encoding='utf-8') as infile, \
     open(output_file, 'w', encoding='utf-8') as outfile:

    for line in infile:
        word = line.strip() # Removes \n, \r, and trailing spaces

        if not word:
            continue

        if pattern.match(word):
            outfile.write(word + '\n')
            kept_count += 1
        else:
            removed_count += 1

print("Done!")
print(f"Kept:    {kept_count} words.")
print(f"Removed: {removed_count} words.")
print(f"Saved to: {output_file}")
print("(You can now rename this file to replace your old dictionary!)")
