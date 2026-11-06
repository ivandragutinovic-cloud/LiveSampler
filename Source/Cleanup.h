#pragma once
#include "PluginEditor.h"

// SETTINGS -> CLEANUP: presemplovani Kronos ROM / EXi zvuci koje nijedan user ne koristi (brisanje u Recycle Bin,
// po izboru, sa preslusavanjem) i useri koji nisu ni u jednom performansu.
class CleanupPanel : public juce::Component, private juce::ListBoxModel, private juce::Timer, private juce::Thread
{
public:
    explicit CleanupPanel (LiveSamplerProcessor&);
    ~CleanupPanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    static void show (LiveSamplerProcessor&);

private:
    struct Item
    {
        juce::String name, kind, info;
        juce::File folder;
        juce::int64 bytes = -1;      // -1 = jos se racuna
        int userIndex = -1;
        bool checked = false;
    };

    LiveSamplerProcessor& proc;
    bool usersMode = false;
    std::vector<Item> items;
    juce::CriticalSection itemsLock;
    juce::TextButton samplesTab, usersTab, allButton, noneButton, rescanButton, deleteButton;
    juce::Label title, summary, hint;
    juce::ListBox list { {}, this };
    int playing = -1;                 // red koji se presluskuje
    int noteOffAt = 0;
    juce::String usedSnapshotKey;

    void rebuild();
    void updateSummary();
    void togglePlay (int row);
    void stopPlay();
    void deleteSelected();
    bool playSampleFolder (const juce::File& folder);

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void timerCallback() override;
    void run() override;              // velicine foldera (pozadina)
};
