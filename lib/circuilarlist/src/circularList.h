#include "Arduino.h"
#include <math.h>

#ifndef __BOONDOCK_CIRCULAR_LIST__
#define __BOONDOCK_CIRCULAR_LIST__

// Metadata captured for a recording
struct RecordingMetadata
{
    String filename;
    String endReason;
    int duration;
    float decibel;
    int size;
    bool hasValue;

    RecordingMetadata() : duration(-1), decibel(NAN), size(-1), hasValue(false) {}
};

// Node for circular linked list
class Node
{
public:
    String data;
    Node *next;
    Node *prev;
    bool played; // Add a flag to track whether the file has been played or not
    String endReason;
    int duration;
    float decibel;
    int fileSize;
    bool hasMetadata;

    Node(String data, const String &endReason = "", int duration = -1, float decibel = NAN, int fileSize = -1)
        : data(data), next(nullptr), prev(nullptr), played(false), endReason(endReason), duration(duration), decibel(decibel), fileSize(fileSize)
    {
        hasMetadata = endReason.length() > 0 || duration >= 0 || fileSize >= 0 || !isnan(decibel);
    }

    void setMetadata(const String &newEndReason, int newDuration, float newDecibel, int newFileSize)
    {
        endReason = newEndReason;
        duration = newDuration;
        decibel = newDecibel;
        fileSize = newFileSize;
        hasMetadata = newEndReason.length() > 0 || newDuration >= 0 || newFileSize >= 0 || !isnan(newDecibel);
    }
};

// Circular linked list to manage audio recordings
class CircularList
{
private:
    Node *head;
    Node *tail;
    Node *current; // Pointer to the last played file
    int size;
    int capacity;
    int audioIndex; // Index of the last played audio

public:
    CircularList(int capacity) : head(nullptr), tail(nullptr), current(nullptr), size(0), capacity(capacity), audioIndex(-1) {}


    // Add a new recording file name to the list
    void add(String filename, const String &endReason = "", int duration = -1, float decibel = NAN, int size = -1);

    // Play the next audio file
    String getNextAudio(bool ignorePlayed);

    // Play the previous audio file
    String getPrevAudio();

    // Function to get the last audio file
    String getLastAudio();

    // Function to get the first audio file
    String getFirstAudio();


    // Function to get the first audio file
    String getMailboxJson();

    // Retrieve stored metadata for a recording
    bool getMetadata(const String &filename, RecordingMetadata &metadata) const;

    // Update metadata for an existing recording without adding a new node
    void updateMetadata(const String &filename, const String &endReason, int duration, float decibel, int size);

    // Function to get the count of current recordings in memory
    int getCount();

    // Function to get the count of unplayed messages
    int getNewCount();

    // Function to get the count of current recordings in memory
    int getNewCount(CircularList &circularList);

    // Function to remove a recording from the list
    void remove(String filename);

    // Function to get the last played audio file
    String getLastPlayedAudio();

    // Function to get the index of the last played audio
    int getLastPlayedIndex();

    // Function to select the next unplayed message
    String getNextUnplayedMessage();

    // Function to fetch a record at a specific index
    String getAudioAtIndex(int index, bool unPlayed);

    // Function to set the current index to a specific value
    void setCurrentIndex(int index);
};

#endif
