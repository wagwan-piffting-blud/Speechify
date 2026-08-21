# build_tools — everything the CRS Mara recipe needs

The recipe in `../README.md` names these by filename only, because this is where
they are. Nothing here reaches outside the repository except `$Work`, the
scratch directory you pick for build arms.

    paths.py            path anchors; `python paths.py` reports what resolves
    vb_pickmap.py       which units the selector actually chooses
    vb_demandmap.py     which preselection contexts real speech asks for
    vb_keepspans.py     turns those two into the --compress keep list
    vb_vcfarm.py        a VCF-only copy of a voice, for one-variable tests
    vb_shipvcf.py       adds the keys --vcf-set refuses; writes the ship VCF
    vb_vin.py           VIN/VDB RIFF reader (XOR 0xCE)
    vb_ckls.py          the whole-word / whole-syllable anchor index
    vb_prsl.py          the preselection table
    vb_listen.py        voice discovery by name over en-US/
    vcf_variant.py      the VCF nibble cipher, and what may be added to one
    data/               the word lists and the demand sample (see below)

Every `vb_*.py` and `vcf_variant.py` is a byte-identical copy of the original
under `reveng/spfy4/tools/`. **`paths.py` is the one file that differs**: the
originals sit four directories below the repo root and this sits three, so it
is a depth-corrected stand-in that exports the same names. If you change a tool
here, change it there too, or the two will drift.

## Running them

Python 3.12 with `numpy`. From `en-US/crsmara/`:

```powershell
python build_tools\paths.py          # what resolves on this machine
python build_tools\vb_keepspans.py --help
```

Machine-specific locations are environment variables with defaults, so no file
here needs editing:

    SPFY4_SCRATCH   working directory for build arms      default C:\tmp
    SPFY4_BUILD     the built spfy CLI tree               default <scratch>\spfy_build32
    SPFY4_CRS       the audio archive, if you have one    default D:\__crs

## data/

    keep_words_wx.txt      words the cut must keep WHOLE. ⭐ Load-bearing:
                           `tornado` has 13 word anchors and the demand sample
                           touches none, so without this line the builder drops
                           all 13 and the word gets spliced out of half-phones.
                           214 units, 0.09 MB
    gate_rare_both.txt     words withheld from preselection — mostly rare
                           non-English tokens the front end mispronounces, whose
                           units then leak into ordinary words
    drop_it2.txt           recordings withheld outright
    texts_brown.txt        2,588 general-prose lines. The recipe uses `--skip
                           1000`, so 1,588 of them are held out of the corpus
    arpa_fekey_px2.dict    9,511 headwords, 0 OOV. ⚠ keyed on the FE spelling,
                           not the .txt word

⚠ **Held out means held out.** Text folded into the corpus asks for exactly the
contexts that text created; measuring demand on it protects the material that
was just added and calls the circularity a result.

## What is NOT here

The corpus — 9,686 recordings and their TextGrids, several gigabytes. The
recipe's steps 1 and 5 need it; steps 2 through 4 only need a built voice.
