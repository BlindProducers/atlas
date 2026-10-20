# Atlas: Library Layout Plan

Plan as of 2026-10-20, agreed in an interview and revised the same day. Work happens on the `browser-and-ui-fixes` branch.

Each library holds its banks and one folder per kind of file. A bank is a set of presets. Wavetables, multisamples, samples, LFO presets, FX presets and impulse responses live in their library's kind folders, as packs or loose files.

## The layout

```
<Atlas folder>/
  Factory/        content Atlas ships
  User/           what the user saves and keeps
  Third Party/    banks and packs imported from files or dropped in by hand
  Community/      downloads from atlas.blindproducers.org
    Banks/
      <Bank>/<category folders>/<preset>.atp
    Wavetables/<pack>/<folders>/<file>
    Multisamples/<instrument>/<the instrument's own files>
    Samples/<pack>/...
    LFO Presets/<pack>/...
    FX Presets/<pack>/...          .atf and .atfx
    Impulse Responses/<pack>/...
```

- The four library folders are the only things at the top level besides Atlas's own files (AtlasPresetIndex.db, Migration.txt, errors.txt, packs.json).
- A bank is a folder in Banks. The folders inside it are its presets' categories, and they may nest (Bass/Sub).
- The wavetables, samples, LFO and FX presets and impulse responses a bank ships with become a pack named after the bank in each kind folder. Third Party/Banks/Virtual Riot Vol 1 holds the presets, and Third Party/Wavetables/Virtual Riot Vol 1 holds its tables.
- A multisample instrument lives once in its library's Multisamples folder, however many presets in the library play it.
- Community also keeps Archives (the bank files it downloaded) and Previews (the audio previews it fetched).
- The preset browser's rows are Collection, Library (Factory, User, Third Party, Community), Bank, Category and Tags. Every Browse menu shows library, then pack and folders, then file.
- The default patch is User/Default.atp, outside every bank.
- The default bank is the bank last saved into, kept in the config, and My Bank until there is one. A first Save patch and Scan Downloads use it. LFO, FX, wavetable and impulse response Save dialogs open in the User library's kind folders.

## Decisions made in the interview

1. One layout for every library, Factory and User included, reached by migration.
2. Saving a preset whose instrument came from outside the library puts the instrument in the library's Multisamples folder, by hard link where the files are on the same volume, by copy otherwise.
3. Save (Command S) overwrites in place only inside User. On a Factory, Third Party or Community preset it opens Save as.
4. The migration collapses Factory's presets into two banks: Atlas for the repository's content (Oscillator Slots) and Vital for everything else. Factory's packs keep their own names. The user's Factory content is test material.
5. A community bank is filed under its own name. Loose community presets keep the uploader's account name as their bank.
6. Every kind gets the same Browse menu. Load from file stays everywhere.
7. The browser gets six bank actions: New bank, Rename bank, Delete bank, Move bank to another library, Export bank and Share bank from the Bank row, and Reveal bank.
8. The instrument folder is called Multisamples.
9. Revised: banks hold presets only, in a Banks folder. Every other kind sits beside Banks in its own library folder, named Wavetables, Multisamples, Samples, LFO Presets, FX Presets and Impulse Responses. This replaced the first version, where every folder of a kind became a bank of its own, which put wavetable libraries beside preset banks.

## Assumptions made without asking

- A bank's own wavetables, samples, LFO and FX presets become packs named after the bank, and Export bank gathers them back.
- A preset leaves a bank on its own only through Export patch as preset file and Share patch, which keep building the self-contained zip. A zip preset found in a bank, by a preset scan or after a community download, has its instruments unpacked into the library once and becomes plain JSON.
- Importing a bank whose name exists already merges. Files in the archive replace same-named files, everything else stays, and Atlas says how many were replaced. An instrument the library has already, by hash, is not added again.
- An archive without presets, a wavetable pack for example, is filed as packs only.
- The migration runs once, when the editor first opens in a host, without asking. It writes Migration.txt and announces what it did. Each move is a folder rename.
- Additional wavetable and sample folders from the config stay as extra roots at the end of their Browse menus.
- Bank archives keep Vital's layout inside (Presets, Wavetables, LFOs, FX, Samples, Impulse Responses, Multisamples), so older Atlas builds, Vital and the community server read them.

## Migration

Applied when the config's `library_layout` is below 3. Every move is a rename within the Atlas folder.

From the first layout, where each library held kind folders:
- `<Library>/Presets/<X>/...` becomes `<Library>/Banks/<X>/...`. Loose user presets go to the default bank, named after the author they agree on. In Factory, Oscillator Slots goes to Banks/Atlas, and the rest goes to Banks/Vital.
- `<Library>/LFOs` becomes `LFO Presets`, and `FX` becomes `FX Presets`. Wavetables, Samples and Impulse Responses are already in place.
- `User/Presets/Default.atp` becomes `User/Default.atp`.
- `Community/Banks`, which held downloaded archives, becomes `Community/Archives` before anything else moves.

From the interim layout, where each library held `<Bank>/<Kind>` folders:
- `<Bank>/Presets` becomes `Banks/<Bank>`.
- `<Bank>/<Kind>` becomes `<Kind>/<Bank>`, a pack of the bank's name.
- Factory's banks and the default User bank held loose files, so their contents go straight into the kind folders. Factory/Samples/Tape is where the Tape effect looks.
- `<Bank>/Multisamples/<instrument>` becomes `Multisamples/<instrument>`.

In both cases, bank-shaped folders at the top of the Atlas folder are filed into Third Party, or into Community when its archive came from there. Favourites are rewritten, the preset index is rebuilt by the next scan, and the moves are logged.

On the user's Mac, a rehearsal on a copy of the library's tree made 81 moves with no problems. User keeps 8 banks and holds 27 wavetable packs, 3 sample packs, 3 impulse response packs, and the Salamander piano in Multisamples. Third Party holds 11 banks, Virtual Riot's wavetables and KLING's LFO presets.

## Multisamples

- An instrument Atlas places is a folder in the library's Multisamples, holding the instrument's files in their own layout beside `atlas-multisample.json`. The manifest records the hash, the entry, the format, the files with sizes and CRCs, and the paths mapped from outside the instrument.
- A preset in a bank names its instrument by `library_instrument`, the entry file's path in Multisamples, beside the path and hash. Loading looks in the preset's library first, then at the path, then in every library by hash, then in the cache.
- An instrument the user put in Multisamples is referred to where it is. A library's instruments are never copied into the cache.

## Phases

1. Done: the library model and migration, the browser rows, the Save form's bank list, import and export landing, and the favourites rewrite.
2. Done: multisamples in the library, with manifests, references, placing on save, unpacking containers, hash lookup on restore and the bank export round trip.
3. Done: Browse menus for LFO presets, FX presets and instruments. Each effect slot has an FX presets group and each LFO an LFO presets group, holding Browse, Previous, Next, Import and Save. Browse lists the presets for that effect only, by each file's effect field, from every library. The bus group gained Browse bus FX presets, and Multi-sample slots gained Browse instruments.
4. Bank actions and the Edit metadata bank list.

## Tests

- `AtlasMacroTests --library-layout`: paths, the folders a library lists, migration from both older layouts, stray banks, bank and pack archives written and imported, and the preset Browse lists, with FX presets filtered by effect, loaded into a slot, and stepped with Previous and Next.
- `AtlasMultiSampleTests --bank`: instruments placed on save, shared by a library's banks, found by reference and by hash, unpacked from a container, and exported and imported with their bank.
- `AtlasMacroTests --community-library`: the download and bank landing paths.
- `AtlasMacroTests --library-migration <folder> <layout> [default bank]`: by hand, migrates a copy of a real Atlas folder and prints the log.
- Test binaries never reorganise a real library: the migration and the scan's filing run only when a host loaded the plugin.

## Risks

- Windows path length: Banks adds a level. Imports and copies of deep instruments can pass 260 characters, which fails in hosts without a long-path manifest.
- An older Atlas build sharing the folder finds nothing after the migration, and recreates empty User/Presets folders that the next migration removes. Betas only.
- Favourites and the preset index name paths, and both are rewritten. DAW projects name instrument paths, and those are found through the hash.
- Hard links fail on exFAT and across volumes, so placing falls back to a copy.
