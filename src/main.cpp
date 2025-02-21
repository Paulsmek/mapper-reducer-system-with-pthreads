#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <pthread.h>
#include <mutex>
#include <algorithm> 

// Structure for thread data
struct ThreadData {
    int thread_id;
    // Both nr of threads are needed for reducer threads
    int num_threads_map;
    int num_threads_reduce;
    size_t *last_file_nr; // The next file available
    pthread_mutex_t *file_mutex;
    pthread_barrier_t *barrier;
    std::vector<std::map<std::string, std::set<int>>> *local_maps;
    std::vector<std::string> *files;
};

// Convert to lowercase if alphabetic
char normalize_char(char ch) {
    if (std::isalpha(ch)) {
        return std::tolower(ch);
    }
    return '\0'; // Ignored chars
}

// Function for Mapper threads to process files
void *mapper_thread(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    int thread_id = data->thread_id;

    while (true) {
        size_t local_file_index;

        // Lock the file mutex to safely get the next file index
        pthread_mutex_lock(data->file_mutex);
        if ((*data->last_file_nr) >= (*data->files).size()) {
            pthread_mutex_unlock(data->file_mutex); // Unlock and exit if no files left
            break;
        }
        local_file_index = (*data->last_file_nr)++;
        pthread_mutex_unlock(data->file_mutex); // Unlock after updating the index

        // Process the file
        std::ifstream file((*data->files)[local_file_index]);
        std::string buffer, normalized_word;

        while (file >> buffer) {
            normalized_word.clear();
            // Add all the good chars to the word
            for (char ch : buffer) {
                char normalized = normalize_char(ch);
                if (normalized) {
                    normalized_word += normalized;
                }
            }

            if (!normalized_word.empty()) {
                // Add word to local map
                (*data->local_maps)[data->thread_id][normalized_word].insert(local_file_index + 1);
            }
        }
        file.close();
    }
    // "Signal" that the mapper is done with the files 
    pthread_barrier_wait(data->barrier);
    return nullptr;
}

bool comparator(const std::pair<std::string, std::set<int>> &a, 
                const std::pair<std::string, std::set<int>> &b) {
    // Sort by the number of files (descending)
    if (a.second.size() != b.second.size()) {
        return a.second.size() > b.second.size();
    }
    // If equal, sort alphabetically
    return a.first < b.first;
}

// Function for Reducer threads to write output
void *reducer_thread(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    int thread_id = data->thread_id;

    pthread_barrier_wait(data->barrier); // Wait for all mappers to finish

    // Combine thread-local maps into a local combined map
    std::map<std::string, std::set<int>> combined_map;

    // Collect words for this threads responsibility
    for (int t = 0; t < data->num_threads_map; t++) {
        for (const auto &entry : (*data->local_maps)[t]) {
            char first_letter = entry.first[0];
            // Make sure only this threads specific letters are used  
            if (first_letter >= 'a' + thread_id && first_letter <= 'z' &&
                (first_letter - 'a') % data->num_threads_reduce == thread_id) {
                combined_map[entry.first].insert(entry.second.begin(), entry.second.end());
            }
        }
    }

    // Sort combined_map entries using compare function
    std::vector<std::pair<std::string, std::set<int>>> sorted_entries(combined_map.begin(), combined_map.end());
    std::sort(sorted_entries.begin(), sorted_entries.end(), comparator);

    // Write sorted results to this threads files 
    for (char start_letter = 'a' + thread_id; start_letter <= 'z'; start_letter += data->num_threads_reduce) {
        // Convert char to string to support "+" operation
        std::string file_path =  std::string(1, start_letter) + ".txt";
        std::ofstream output_file(file_path);

        for (const auto &entry : sorted_entries) {
            if (entry.first[0] == start_letter) {
                output_file << entry.first << ":[";

                auto it = entry.second.begin();
                while (it != entry.second.end()) {
                    output_file << *it;
                    if (++it != entry.second.end()) output_file << " ";
                }

                output_file << "]\n";
            }
        }
        output_file.close();
    }

    return nullptr;
}

int main(int argc, char *argv[]) {
    // 4 arguments required
    if (argc != 4) {
        return 1;
    }
    int num_mappers = std::stoi(argv[1]);
    int num_reducers = std::stoi(argv[2]);
    std::string input_file = argv[3];

    // Read the input file list
    std::ifstream input(input_file);
    std::vector<std::string> files;
    // Local maps for mapper threads
    std::vector<std::map<std::string, std::set<int>>> local_maps(num_mappers);

    int num_files;
    size_t last_file = 0;
    pthread_mutex_t file_mutex;
    pthread_barrier_t barrier;
    input >> num_files;
    input.ignore(); // Skip the newline

    // Resize vectors for efficiency
    files.resize(num_files);
    local_maps.resize(num_mappers);
    for (int i = 0; i < num_files; i++) {
        std::getline(input, files[i]);
    }
    input.close();

    // Initiate mutex and barrier
    pthread_mutex_init(&file_mutex, NULL);
    pthread_barrier_init(&barrier, NULL, num_mappers + num_reducers);
    
    // Vectors for threads and data
    std::vector<pthread_t> threads(num_mappers + num_reducers);
    std::vector<ThreadData> threads_data(num_mappers + num_reducers);

    // Create all mapper and reducer threads in an iteration
    for (int i = 0; i < num_mappers + num_reducers; i++) {
        if (i < num_mappers) {
            threads_data[i] = {i, num_mappers, num_reducers, &last_file, &file_mutex, &barrier, &local_maps, &files};

            if (pthread_create(&threads[i], nullptr, mapper_thread, &threads_data[i])) {
                std::cerr << "Error creating Mapper thread " << i << std::endl;
                return 1;
            }
        }
        else {
            threads_data[i] = {i - num_mappers, num_mappers, num_reducers, &last_file, &file_mutex, &barrier, &local_maps, &files};
            if (pthread_create(&threads[i], nullptr, reducer_thread, &threads_data[i])) {
                std::cerr << "Error creating Reducer thread " << i << std::endl;
                return 1;
            }
        }
    }

    // Join all threads
    for (int i = 0; i < num_mappers + num_reducers; i++) {
        if (pthread_join(threads[i], nullptr)) {
            std::cerr << "Error joining thread " << i << std::endl;
            return 1;
        }
    }
    // Destory mutex and barrier
    pthread_mutex_destroy(&file_mutex);
    pthread_barrier_destroy(&barrier);
    return 0;
}
