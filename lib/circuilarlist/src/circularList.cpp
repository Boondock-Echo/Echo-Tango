#include "circularList.h"
#include "ArduinoJson.h"
// Implement the member functions of CircularList class here

// Add a new recording file name to the list
void CircularList::add(String filename, const String &endReason, int duration, float decibel, int size)
{
    if (head)
    {
        Node *existing = head;
        do
        {
            if (existing->data == filename)
            {
                existing->setMetadata(endReason, duration, decibel, size);
                return;
            }
            existing = existing->next;
        } while (existing != head);
    }

    Node *newNode = new Node(filename, endReason, duration, decibel, size);

    if (size == 0)
    {
        head = newNode;
        tail = newNode;
        newNode->next = head;
        newNode->prev = tail;
        current = head; // Initialize current to head
        audioIndex = 0; // Initialize audioIndex when adding the first audio
    }
    else
    {
        tail->next = newNode;
        newNode->prev = tail;
        newNode->next = head;
        head->prev = newNode;
        tail = newNode;
    }

    if (size == capacity)
    {
        // Remove the oldest element to make space for the new one
        Node *temp = head;
        head = head->next;
        tail->next = head;
        head->prev = tail;
        delete temp;
        if (current == temp)
        {
            current = head;
            audioIndex = 0; // Update audioIndex to reflect the new current
        }
    }
    else
    {
        size++;
    }
}

String CircularList::getNextAudio(bool ignorePlayed)
{
    if (head)
    {
        Node *currentNode = head;

        // Iterate through the circular list, starting from the beginning
        while (currentNode != tail)
        {
            if (ignorePlayed || !currentNode->played)
            {
                // If ignorePlayed is true or an unplayed message is found, update current and audioIndex
                current = currentNode;
                audioIndex = 0; // Reset audioIndex since we are not wrapping around
                if (!ignorePlayed)
                {
                    currentNode->played = true; // Mark the message as played if ignorePlayed is false
                }
                return currentNode->data;
            }
            currentNode = currentNode->next;
        }

        // Check the last node (tail)
        if (ignorePlayed || !currentNode->played)
        {
            current = currentNode;
            audioIndex = 0; // Reset audioIndex since we are not wrapping around
            if (!ignorePlayed)
            {
                currentNode->played = true; // Mark the message as played if ignorePlayed is false
            }
            return currentNode->data;
        }
    }

    // If all messages are played or the list is empty, return an empty string
    return "";
}

String CircularList::getMailboxJson()
{
    DynamicJsonDocument doc(1024); // Adjust the size based on your needs
    JsonArray array = doc.to<JsonArray>();

    // Temporary pointer to iterate through the list
    Node *temp = head;

    // Iterate through the circular list
    if (temp != nullptr)
    {
        do
        {
            JsonObject nodeObject = array.createNestedObject();
            nodeObject["data"] = temp->data;
            nodeObject["played"] = temp->played;

            // Move to the next node
            temp = temp->next;
        } while (temp != head); // Check for the end of the list
    }

    // Convert JSON document to string
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

bool CircularList::getMetadata(const String &filename, RecordingMetadata &metadata) const
{
    metadata.hasValue = false;
    if (!head)
    {
        return false;
    }

    Node *currentNode = head;
    do
    {
        if (currentNode->data == filename && currentNode->hasMetadata)
        {
            metadata.filename = currentNode->data;
            metadata.endReason = currentNode->endReason;
            metadata.duration = currentNode->duration;
            metadata.decibel = currentNode->decibel;
            metadata.size = currentNode->fileSize;
            metadata.hasValue = true;
            return true;
        }
        currentNode = currentNode->next;
    } while (currentNode != head);

    return false;
}

void CircularList::updateMetadata(const String &filename, const String &endReason, int duration, float decibel, int size)
{
    if (!head)
    {
        return;
    }

    Node *currentNode = head;
    do
    {
        if (currentNode->data == filename)
        {
            currentNode->setMetadata(endReason, duration, decibel, size);
            return;
        }
        currentNode = currentNode->next;
    } while (currentNode != head);
}

// Play the previous audio file
String CircularList::getPrevAudio()
{
    if (head && current)
    {
        if (current->prev != head) // Check if the previous node is not the head
        {
            current = current->prev;
            audioIndex--;

            if (audioIndex < 0)
            {
                audioIndex = 0;
            }

            return current->data;
        }
    }

    // If it reaches the beginning of the list or there are no previous messages, return an empty string
    return "";
}

// Function to get the last audio file
String CircularList::getLastAudio()
{
    // Implement the code here
    return "";
}

// Function to get the first audio file
String CircularList::getFirstAudio()
{
    // Implement the code here
    return "";
}

// Function to get the count of current recordings in memory
int CircularList::getCount()
{
    return size;
}

// Function to get the count of unplayed messages
int CircularList::getNewCount()
{
    int unplayedCount = 0;
    Node *currentNode = head;

    // Iterate through the circular list
    do
    {
        if (!currentNode->played)
        {
            // If the 'played' flag is false, increment the unplayed count
            unplayedCount++;
        }
        currentNode = currentNode->next;
    } while (currentNode != head);

    return unplayedCount;
}

// Function to get the count of current recordings in memory
int CircularList::getNewCount(CircularList &circularList)
{
    // Implement the code here
    return 0;
}

// Function to remove a recording from the list
void CircularList::remove(String filename)
{
    Node *currentNode = head;
    if (currentNode)
    {
        do
        {
            if (currentNode->data == filename)
            {
                // Found the node with the specified filename
                if (currentNode == current)
                {
                    // If the node to remove is the current, update current
                    current = current->next;
                    audioIndex--;
                    if (audioIndex < 0) // Wrap around if we reach the beginning
                    {
                        audioIndex = size - 1;
                    }
                }

                if (currentNode == head)
                {
                    // If the node to remove is the head, update head
                    head = head->next;
                }

                if (currentNode == tail)
                {
                    // If the node to remove is the tail, update tail
                    tail = tail->prev;
                }

                currentNode->prev->next = currentNode->next;
                currentNode->next->prev = currentNode->prev;
                delete currentNode;
                size--;
                break;
            }
            currentNode = currentNode->next;
        } while (currentNode != head);
    }
}

// Function to get the last played audio file
String CircularList::getLastPlayedAudio()
{
    // Implement the code here
    return "";
}

// Function to get the index of the last played audio
int CircularList::getLastPlayedIndex()
{
    // Implement the code here
    return -1;
}

// Function to select the next unplayed message
String CircularList::getNextUnplayedMessage()
{
    // Implement the code here
    return "";
}

// Function to fetch a record at a specific index
String CircularList::getAudioAtIndex(int index, bool unPlayed)
{
    if (index >= 0 && index < size)
    {
        Node *currentNode = head;
        for (int i = 0; i < index; i++)
        {
            currentNode = currentNode->next;
        }
        current = currentNode;
        audioIndex = index;
        if (current->played && unPlayed)
            return "";
        else
        {
            currentNode->played = true;
            return currentNode->data;
        }
    }
    else
    {
        return ""; // Return an empty string for an invalid index
    }
}

// Function to set the current index to a specific value
void CircularList::setCurrentIndex(int index)
{
    // Implement the code here
}
