// SPDX-License-Identifier: MIT
//
// Copyright (c) 2021-2022, Fewnity - Grégory Machefer
//
// This file is part of Counter Strike Nintendo DS Multiplayer Edition (CS:DS)

#include "sounds.h"
#include "player.h"
#include <maxmod9.h>
#include <fat.h>
#include <filesystem.h>
#include <unistd.h>  // access()/F_OK, no longer pulled in transitively

// Recently started sound effects, released oldest-first. Fewer than the number of
// hardware channels, so there is always one free to allocate.
#define TrackedEffectCount 8
static mm_sfxhand recentEffects[TrackedEffectCount];
static int oldestEffect = 0;

/**
 * @brief Keep a sound effect's channel reserved for a while, then release it
 *
 * @param handle Handle returned by mmEffect, or 0 if the effect did not start
 */
static void trackEffect(mm_sfxhand handle)
{
    if (handle == 0)
        return;
    if (recentEffects[oldestEffect] != 0)
        mmEffectRelease(recentEffects[oldestEffect]);
    recentEffects[oldestEffect] = handle;
    oldestEffect = (oldestEffect + 1) % TrackedEffectCount;
}

/**
 * @brief Forget tracked handles, for when every effect has been cancelled
 */
static void forgetEffects()
{
    for (int i = 0; i < TrackedEffectCount; i++)
        recentEffects[i] = 0;
    oldestEffect = 0;
}

// maxmod resolves mmEffect(id) as mem_bank[mod_count + id], masks the word to 24 bits
// and adds 0x02000000, with no bounds check of its own: a bad id makes the ARM7 play
// arbitrary main RAM. There are no songs in this soundbank, so mod_count is 0 and the
// id indexes the table directly.
static mm_word *sampleBank = NULL;

/**
 * @brief Play a sample, refusing anything maxmod would turn into arbitrary memory
 *
 * @param sound Sample id
 * @return Handle from maxmod, or 0 if nothing started
 */
static mm_sfxhand startEffect(mm_word sound)
{
    if (sound >= (mm_word)MSL_NSAMPS)
        return 0;
    if (sampleBank && sampleBank[sound] == 0)
        return 0;

    return mmEffect(sound);
}

// Music remaining time
int musicLength = 0;
// Is music playing
bool isMusicPlaying = false;
// Are sounds loaded
bool soundsLoaded = false;
// Are sound bank file loaded
bool soundBankLoaded = false;
// Is music file is accessible
bool musicFilePresent = false;

// Music file
FILE *musicFile = NULL;

/**
 * @brief Stream callback for maxmod
 *
 * @param length remaining streaming time
 * @param dest
 * @param format
 * @return mm_word
 */
mm_word stream(mm_word length, mm_addr dest, mm_stream_formats format)
{
    if (musicFile)
    {
        size_t samplesize;
        switch (format)
        {
        case MM_STREAM_8BIT_MONO:
            samplesize = 1;
            break;
        case MM_STREAM_8BIT_STEREO:
            samplesize = 2;
            break;
        case MM_STREAM_16BIT_MONO:
            samplesize = 2;
            break;
        case MM_STREAM_16BIT_STEREO:
            samplesize = 4;
            break;
        default:
            samplesize = 2;
            break;
        }

        int res = fread(dest, samplesize, length, musicFile);

        if (res)
        {
            length = res;
        }
        else
        {
            mmStreamClose();
            fclose(musicFile);
            musicFile = NULL;
            length = 0;
        }
    }
    musicLength = length;
    return length;
}

/**
 * @brief Check if music is finished to restart the music
 *
 */
void checkMusicSteaming()
{
    if (isMusicPlaying)
    {
        // If the music is finished, restart it
        if (musicLength == 0)
        {
            loadMusic();
        }
        else // Or update stream
        {
            mmStreamUpdate();
        }
    }
}

/**
 * @brief Init sound system and load sound bank and music
 *
 */
void initSoundSystem()
{
    // Init sound system
    // mmInitDefaultMem((mm_addr)soundbank_bin);
    mm_ds_system sys;

    // number of modules in your soundbank (defined in output header)
    sys.mod_count = MSL_NSONGS;

    // number of samples in your soundbank (defined in output header)
    sys.samp_count = MSL_NSAMPS;

    // memory bank, allocate BANKSIZE (or NSONGS+NSAMPS) words
    sys.mem_bank = malloc(MSL_BANKSIZE * 4);
    sampleBank = (mm_word *)sys.mem_bank;

    // This table MUST start zeroed. maxmod treats an entry whose top byte is non-zero as
    // "this sample is already loaded", so leftover heap content makes mmLoadEffect skip
    // the sample and leave the entry as a garbage 24-bit offset from 0x02000000 -- which
    // the ARM7 then plays as if it were PCM. maxmod's own mmInitDefault calloc's this for
    // exactly that reason; malloc only worked while the heap happened to still be fresh.
    for (int i = 0; i < MSL_BANKSIZE; i++)
        sampleBank[i] = 0;

    // select fifo channel
#if _LIBNDS_MAJOR_ >= 2
    sys.fifo_channel = 0; // Calico Maxmod uses PXI; this compatibility field is unused.
#else
    sys.fifo_channel = FIFO_MAXMOD;
#endif

    // initialize maxmod
    mmInit(&sys);

    //
    if (access("fat:/counter_strike_music.raw", F_OK) == 0 || access("counter_strike_music.raw", F_OK) == 0 || access("sd:/counter_strike_music.raw", F_OK) == 0)
    {
        musicFilePresent = true;
    }
    else
    {
        musicFilePresent = false;
    }

    // Hand maxmod the path that actually exists. The relative one resolves against the
    // working directory, which is wherever the running .nds lives, so it is only right
    // when the soundbank sits beside the build.
    static const char *const soundBankPaths[] = {"fat:/soundbank.bin", "soundbank.bin", "sd:/soundbank.bin"};
    soundBankLoaded = false;
    for (unsigned i = 0; i < sizeof(soundBankPaths) / sizeof(soundBankPaths[0]); i++)
    {
        if (access(soundBankPaths[i], F_OK) == 0)
        {
            mmSoundBankInFiles(soundBankPaths[i]);
            soundBankLoaded = true;
            break;
        }
    }
}

/**
 * @brief Start the music
 *
 */
void launchMusic()
{
    if (!isMusicPlaying)
    {
        isMusicPlaying = true;
        unloadSounds();
        loadMusic();
    }
}

/**
 * @brief Close music stream
 *
 */
void closeMusicSteam()
{
    if (musicFile)
    {
        mmStreamClose();
        fclose(musicFile);
        musicFile = NULL;
    }
}

/**
 * @brief Load music file and open stream
 *
 */
void loadMusic()
{
    if (!musicFilePresent)
        return;

    closeMusicSteam();
    // open file
    musicFile = fopen("fat:/counter_strike_music.raw", "rb");
    if (musicFile == NULL)
    {
        musicFile = fopen("counter_strike_music.raw", "rb");
        if (musicFile == NULL)
        {
            musicFile = fopen("sd:/counter_strike_music.raw", "rb");
        }
    }

    // Open stream
    if (musicFile)
    {
        mm_stream mystream;
        mystream.buffer_length = 1024;
        mystream.callback = stream;
        mystream.timer = MM_TIMER1;
        mystream.manual = true;
        mystream.sampling_rate = 32000;
        mystream.format = MM_STREAM_8BIT_MONO;
        mmStreamOpen(&mystream);
    }
}

/**
 * @brief Stop music, close stream and load sounds
 *
 */
void stopMusic()
{
    if (isMusicPlaying)
    {
        closeMusicSteam();
        isMusicPlaying = FALSE;
        loadSounds();
    }
}

/**
 * @brief Load sounds from the sound bank and stop the music
 *
 */
void loadSounds()
{
    stopMusic();

    if (soundsLoaded || !soundBankLoaded)
    {
        return;
    }

    soundsLoaded = true;

    // Load sound effects
    for (int i = 0; i < MSL_NSAMPS; i++)
    {
        mmLoadEffect(i);
    }
}

/**
 * @brief Unload sounds
 *
 */
void unloadSounds()
{
    if (!soundsLoaded || !soundBankLoaded)
    {
        return;
    }

    soundsLoaded = false;

    // Stop everything still playing first. Unloading a sample out from under an active
    // channel leaves it reading freed memory, which is heard as noise.
    mmEffectCancelAll();
    forgetEffects();

    // Unload sound effects
    for (int i = 0; i < MSL_NSAMPS; i++)
    {
        mmUnloadEffect(i);
    }

}

/**
 * @brief Play a step sound
 *
 * @param Volume Volume
 * @param Panning Panning (0-255) (left-right)
 * @param playerIndex Player index
 */
void DoStepSound(int Volume, int Panning, int playerIndex)
{
    if (!soundBankLoaded)
        return;

    if (Volume > 0)
    {
        mm_sfxhand mysound;

        // Play random sound
        if (AllPlayers[playerIndex].Step == 0)
            mysound = startEffect(SFX_CONCRETE_CT_1);
        else if (AllPlayers[playerIndex].Step == 1)
            mysound = startEffect(SFX_CONCRETE_CT_2);
        else if (AllPlayers[playerIndex].Step == 2)
            mysound = startEffect(SFX_CONCRETE_CT_3);
        else
            mysound = startEffect(SFX_CONCRETE_CT_4);

        // Set panning and volume
        mmEffectPanning(mysound, Panning);
        mmEffectVolume(mysound, Volume);
        trackEffect(mysound);
        AllPlayers[playerIndex].mapVisivilityTimer = 300;

        AllPlayers[playerIndex].Step++;
        if (AllPlayers[playerIndex].Step == 4)
            AllPlayers[playerIndex].Step = 0;
    }
}

/**
 * @brief Get panning between the local player and a player
 *
 * @param PlayerId Player index
 * @param OutPanning Out Panning (0-255) (left-right)
 * @param OutVolume Out Volume
 * @param xWithoutYForAudio
 * @param zWithoutYForAudio
 * @param MaxSoundDistance Max sound distance
 */
void GetPanning(int PlayerId, int *OutPanning, int *OutVolume, float xWithoutYForAudio, float zWithoutYForAudio, float MaxSoundDistance)
{
    // float MaxSoundDistance = 0.15; //0 = 0 meter; 0.05 = 7.65 meters; 1 = 255 meters
    float CenterOffset = 10;
    float Panning = -1;
    // Callers pass uninitialised locals, and no match used to leave this untouched.
    *OutVolume = 0;

    // With players positions, calculate volume and panning. Index 0 is the local player:
    // sounds about them (being hit, dying) are at the camera, so at full volume.
    for (int i = 0; i < MaxPlayer; i++)
        if (AllPlayers[i].Id == PlayerId)
        {
            if (CurrentCameraPlayer != 0 && CurrentCameraPlayer != i)
                CalculatePlayerPosition(CurrentCameraPlayer);
            CalculatePlayerPosition(i);
            float Dis = sqrtf(squareFloat(AllPlayers[CurrentCameraPlayer].position.x - AllPlayers[i].position.x) + squareFloat(AllPlayers[CurrentCameraPlayer].position.y - AllPlayers[i].position.y) + squareFloat(AllPlayers[CurrentCameraPlayer].position.z - AllPlayers[i].position.z));
            *OutVolume = (int)fmax(255 - (Dis * 2.0) / MaxSoundDistance, 0);
            Panning = (xWithoutYForAudio * (AllPlayers[i].position.x - AllPlayers[CurrentCameraPlayer].position.x)) + (zWithoutYForAudio * (AllPlayers[i].position.z - AllPlayers[CurrentCameraPlayer].position.z));
            break;
        }

    // Panning ajustements
    if (Panning > CenterOffset)
        Panning = CenterOffset;
    else if (Panning < -CenterOffset)
        Panning = -CenterOffset;

    Panning += CenterOffset;
    Panning /= CenterOffset * 2.0;
    Panning *= 255.0;

    // return panning
    *OutPanning = (int)Panning;
}

/**
 * @brief
 * @brief Get panning between the camera and position
 *
 * @param OutPanning Out Panning (0-255) (left-right)
 * @param OutVolume Out Volume (0-255)
 * @param PositionB Position
 * @param xWithoutYForAudio
 * @param zWithoutYForAudio
 * @param MaxSoundDistance Max sound distance
 */
void GetPanningByPosition(int *OutPanning, int *OutVolume, Vector4 PositionB, float xWithoutYForAudio, float zWithoutYForAudio, float MaxSoundDistance)
{
    // float MaxSoundDistance = 0.15; //0 = 0 meter; 0.05 = 7.65 meters; 1 = 255 meters
    float CenterOffset = 10;
    float Panning = -1;

    if (CurrentCameraPlayer != 0)
        CalculatePlayerPosition(CurrentCameraPlayer);

    // With players positions, calculate volume and panning
    float Dis = sqrtf(squareFloat(AllPlayers[CurrentCameraPlayer].position.x - PositionB.x) + squareFloat(AllPlayers[CurrentCameraPlayer].position.y - PositionB.y) + squareFloat(AllPlayers[CurrentCameraPlayer].position.z - PositionB.z));
    *OutVolume = (int)fmax(255 - (Dis * 2.0) / MaxSoundDistance, 0);
    Panning = (xWithoutYForAudio * (PositionB.x - AllPlayers[CurrentCameraPlayer].position.x)) + (zWithoutYForAudio * (PositionB.z - AllPlayers[CurrentCameraPlayer].position.z));

    // Panning ajustements
    if (Panning > CenterOffset)
        Panning = CenterOffset;
    else if (Panning < -CenterOffset)
        Panning = -CenterOffset;

    Panning += CenterOffset;
    Panning /= CenterOffset * 2.0;
    Panning *= 255.0;

    // return panning
    *OutPanning = (int)Panning;
}

/**
 * @brief Play a sound in 3D space
 *
 * @param sound Sound to play
 * @param Volume Volume (0-255)
 * @param Panning Panning (0-255) (left-right)
 * @param player Player pointer
 */
void Play3DSound(mm_word sound, int Volume, int Panning, Player *player)
{
    if (!soundBankLoaded)
        return;

    // If the volume is 0, don't play the sound
    if (Volume > 0)
    {
        mm_sfxhand mysound = startEffect(sound);

        // Set effect panning and volume
        mmEffectPanning(mysound, Panning);
        mmEffectVolume(mysound, Volume);
        trackEffect(mysound);
    }

    // Show the player on the map if the sound is loud
    if (Volume >= 25 && player != NULL)
        player->mapVisivilityTimer = 300;
}

/**
 * @brief Play a sound in 2D space
 *
 * @param sound Sound to play
 * @param Volume Volume (0-255)
 */
void Play2DSound(mm_word sound, int Volume)
{
    if (!soundBankLoaded)
        return;

    // If the volume is 0, don't play the sound
    if (Volume > 0)
    {
        mm_sfxhand mysound = startEffect(sound);

        // Set effeft volume
        mmEffectVolume(mysound, Volume);
        trackEffect(mysound);
    }
}

/**
 * @brief Play a sound
 *
 * @param sound Sound to play
 */
void PlayBasicSound(mm_word sound)
{
    if (!soundBankLoaded)
        return;

    mm_sfxhand handle = startEffect(sound);
    if (handle == 0)
        return;

    // Set the level explicitly. Every sound that is actually audible in this game does,
    // and this is the only path that relied on maxmod's default.
    mmEffectVolume(handle, 255);
    mmEffectPanning(handle, 128);
    trackEffect(handle);
}
