# Speechify 4 Findings

## Firstly, a primer on speech synthesis, because every good lesson is a history lesson...

Speech synthesis is the artificial production of human speech. It involves converting text into spoken words using various techniques and technologies. The goal is to create speech that sounds natural and intelligible to listeners. During the 2002-2015 era (herein "CRS" era, roughly), speech synthesis was primarily based on the principle of **concatenative synthesis**, which involves piecing together pre-recorded speech segments to form complete words and sentences. This method relies on a large database of recorded speech samples, which are then combined to generate new speech output. This is the same method Speechify 3.0.5 uses, and it is what spfy re-implements verbatim, driven by reverse engineering knowledge of the Speechify DLLs. However, Speechify 3 has its limitations. One of the major drawbacks of Speechify 3 is **the lack of pitch modification**, which can make the synthesized speech sound "same-y" or robotic/less empathetic. This is where "Speechify 4 mode" comes in, as it introduces pitch modification to the synthesized speech, aiming to produce a more natural-sounding voice akin to what was used during the CRS era on NOAA Weather Radio.

## What is "Speechify 4", and is/was it even real?

Well, the answer depends on who you ask. If you ask one person, they'll probably say it was real, and can tell the difference between it and Speechify 3, and it's just a relic of the past. But ask a community at large trying to create realistic EAS mocks and simulated NOAA Weather Radio broadcasts, and you'll find that the answer is a bit more nuanced (pun intended). Speechify 4 was a real thing, but it was never released to the public. It was an internal project at SpeechWorks International that was only distributed, seemingly, to a select few clients. The goal of Speechify 4 was to improve the naturalness and expressiveness of the synthesized speech, making it more suitable for applications like NOAA Weather Radio broadcasts. However, Speechify 4 never saw a public release. The only known evidence of its existence is in the form of internal documentation, client communications, and some community discoveries regarding said internal documentation. SpeechWorks International eventually went bust, then got picked up by ScanSoft, later Nuance, then EVEN LATER Microsoft. Now Microsoft owns all the rights and trademarks to Speechify, and they have no interest or intent in releasing Speechify 4 to the public. (They might not even have it anymore.) However, I have taken it upon myself to re-implement Speechify 4 mode in spfy over the course of August 1-11, 2026, and let me tell you _why_.

## The Why:

The why is simple: This community has brought me extensive joy and entertainment over the years, and I want to give back. I want to give back by providing a more natural and realistic-sounding voice for EAS mocks and simulated NOAA Weather Radio broadcasts. It's as simple as that. To the community that has given me so much: I want to make it so that anyone, even the average layperson, can use something just like Speechify 4. Much of the community has spent **years**, in vain, looking for a white whale that may not even be around anymore.

## "You're crazy."

Yes.

## So what does Speechify 4 Mode actually DO?

Under the hood (see [spfy_synth.c](https://github.com/wagwan-piffting-blud/Speechify/blob/main/spfy/src/cli/spfy_synth.c#L71-L204) for the rationale on _why_ these specific settings were chosen):

```c
#define SPFY4_STAGE          "1"
#define SPFY4_ABSOLUTE       "0"
#define SPFY4_DECL_ST        "0"
#define SPFY4_DECL_RATE_ST_S "0"
#define SPFY4_DECL_MAX_ST    "4"
#define SPFY4_FALL_ST        "0"
#define SPFY4_MAX_ST         "12"
#define SPFY4_ZEROMEAN       "1.0"
#define SPFY4_F0_FLOOR_HZ    "95"
#define SPFY4_F0_KNEE_ST     "1.0"
#define SPFY4_RATE           "1.0"
#define SPFY4_PSOLA_SMOOTH   "1"
#define SPFY4_ALIGN_MS       "0"
#define SPFY4_DOWNSTEP_FLOOR "1.0"
#define SPFY4_PSOLA_METHOD   "lp"
```

That's a set of C code parameters that dictate what the engine will do with ANY passed text, and it's... pretty good, in my own personal opinion. But don't let me tell you, judge for yourself:

![Raw NOAA Weather Radio audio](https://github.com/user-attachments/assets/ca7faf1c-ef4a-4d1c-8858-5eb7d405a044)

![spfy in Speechify 4 mode](https://github.com/user-attachments/assets/c75edd48-5176-4303-a414-35c065ff7587)

The exact text synthesized was `\!s4m \![ToBI:H*]This is a radar indicated threat.`. Why the [ToBI](https://en.wikipedia.org/wiki/ToBI) markup? Because it is the only way to get the engine to produce a more correct-sounding intonation on the first word of the sentence. The ToBI markup allows for more precise control over how the synthesized speech sounds, making it possible to achieve a more natural/correct sounding and overall more expressive output. In this case, the ToBI markup `\![ToBI:H*]` indicates that the first word "This" should be stressed with a high pitch accent, which contributes to a more accurate-sounding intonation for the entire sentence. You can also use `\![ToBI:L*]` for a low pitch accent. There's even a `\![ToBI:0]` tag to _suppress_ the FE's accent. Omit the tag to keep the default intonation (aka whatever the frontend produces). ToBI markup is a powerful tool for controlling the prosody of synthesized speech, and it can be used to achieve a wide range of expressive effects, now that prosody can be realized with Speechify 4 mode _at all_. Try it out for yourself, and see what you can do with it. The possibilities are endless, and the results can be quite impressive.

## And what's that `\!s4m` bit in the example?

That's how you turn Speechify 4 mode on for a given sentence.

If you're driving the voice from a SAPI program like Balabolka, you can't pass it command-line flags -- all you can hand it is text. So, naturally, the switch lives **in** the text. Put `\!s4m` at the **very start** of what you want spoken and the tag is used, never read aloud. (`\s4m` without the `!` works too, if you prefer typing less.) There's a few other Speechify 3 control tags like this, they are covered in the [Speechify User's Guide](SpeechifyUsersGuide.pdf) from the time.

Two things about it, both deliberate:

- **It only works at the start.** An `\!s4m` sitting in the middle of a sentence is left completely alone and _will_ be read out loud. That's your hint that it's in the wrong place, rather than it silently doing nothing and leaving you wondering.

- **It only applies to that one SENTENCE.** A tagged sentence won't quietly change every sentence after it. If you need to mix Speechify 3 and 4 modes, save your Speechify 3 text in one sentence, and your Speechify 4 text in another, then use any basic audio editor to join the two. The tag is only for that one sentence, and it will be ignored for any subsequent sentences.

From a command line you can skip the tag entirely: `spfy_synth --s4 ...`, `spfy_synth -4`, or set `SPFY_4_MODE=1` in the environment. Both of those _are_ for the whole run.

## Credits

First and foremost, I must thank two individuals by name: [LafayetteAreaWX](https://www.youtube.com/@LafayetteAreaWX8075/posts) and [JayCLTWXEAS](https://www.youtube.com/@jayCLTeeee). Both of these individuals have been instrumental in making Speechify 4 mode happen in the first place. Lafayette for being able to contribute a _ton_ of direct synthesis from NOAA Weather Radio's archived webpages on Internet Archive (on such short notice too!), and Jay for managing the [CRS Archive](https://drive.google.com/drive/folders/1ADyB0wB8_RL4XS_OQOT6Hdo4fdEA3bc4), a dedicated hub for CRS-era NOAA Weather Radio audio direct from the transmitters. Without their direct contributions, this project would not have been possible.

Secondly, to the overall community that has kept the dream alive for so long. I thank you from the bottom of my heart. Lost media is a fascinating topic in its own right, and after stumbling across Speechify 3 in a VM of all things (which, for many years, was the ONLY way to access Speechify Tom, and was nowhere near accessible to Blind individuals), and not being able to find Speechify 4 anywhere, I decided to take it upon myself to re-implement it. I hope you enjoy it as much as I enjoyed making it happen at all.

And finally, to the original Speechify 4 developers at SpeechWorks International, later ScanSoft if you were part of that transition. I don't know who any of you individuals are, but I hope you know that your work has not been forgotten, and that it has inspired a new generation of enthusiasts to keep the dream alive. Thank you for your contributions to the field of speech synthesis, and for paving the way for future innovations in this area.

And to Microsoft/Microsoft employees on the Nuance side of things... well. You know I want Speechify 4 to be released to the public, but I also know that you have no interest in doing so, if you even have a copy of it yourselves anymore. My [contact page](https://wagspuzzle.space/contact/) remains open if you want to discuss matters, however...
