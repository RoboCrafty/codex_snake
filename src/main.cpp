#include <array>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <termios.h>

namespace
{
    struct Position
    {
        int x {};
        int y {};

        bool operator==(const Position& other) const
        {
            return x == other.x && y == other.y;
        }
    };

    enum class Direction
    {
        Up,
        Down,
        Left,
        Right
    };

    struct GameConfig
    {
        int boardWidth {32};
        int boardHeight {18};
        int minFrameDelayMs {160};
        int maxFrameDelayMs {70};
        int treatsForMaxSpeed {12};
    };

    std::filesystem::path executableDirectory()
    {
        std::array<char, 4096> buffer {};
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length == -1)
        {
            throw std::runtime_error("Failed to resolve executable path.");
        }

        buffer[static_cast<std::size_t>(length)] = '\0';
        return std::filesystem::path(buffer.data()).parent_path();
    }

    std::string trim(const std::string& value)
    {
        std::size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
        {
            ++start;
        }

        std::size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        {
            --end;
        }

        return value.substr(start, end - start);
    }

    int parseInteger(const std::string& key, const std::string& value)
    {
        std::size_t processedCharacters = 0;
        const int parsed = std::stoi(value, &processedCharacters);
        if (processedCharacters != value.size())
        {
            throw std::runtime_error("Invalid integer value for config key: " + key);
        }

        return parsed;
    }

    GameConfig loadConfig(const std::string& filePath)
    {
        GameConfig config;
        std::ifstream configFile(filePath);
        if (!configFile)
        {
            throw std::runtime_error("Failed to open config file: " + filePath);
        }

        std::string line;
        int lineNumber = 0;
        while (std::getline(configFile, line))
        {
            ++lineNumber;
            const std::string cleanedLine = trim(line);
            if (cleanedLine.empty() || cleanedLine[0] == '#')
            {
                continue;
            }

            const std::size_t separator = cleanedLine.find('=');
            if (separator == std::string::npos)
            {
                throw std::runtime_error("Invalid config line " + std::to_string(lineNumber) + ": expected key=value");
            }

            const std::string key = trim(cleanedLine.substr(0, separator));
            const std::string value = trim(cleanedLine.substr(separator + 1));

            if (key == "board_width")
            {
                config.boardWidth = parseInteger(key, value);
            }
            else if (key == "board_height")
            {
                config.boardHeight = parseInteger(key, value);
            }
            else if (key == "min_frame_delay_ms")
            {
                config.minFrameDelayMs = parseInteger(key, value);
            }
            else if (key == "max_frame_delay_ms")
            {
                config.maxFrameDelayMs = parseInteger(key, value);
            }
            else if (key == "treats_for_max_speed")
            {
                config.treatsForMaxSpeed = parseInteger(key, value);
            }
            else
            {
                throw std::runtime_error("Unknown config key: " + key);
            }
        }

        if (config.boardWidth < 10)
        {
            throw std::runtime_error("board_width must be at least 10.");
        }

        if (config.boardHeight < 8)
        {
            throw std::runtime_error("board_height must be at least 8.");
        }

        if (config.minFrameDelayMs <= 0 || config.maxFrameDelayMs <= 0)
        {
            throw std::runtime_error("Frame delay values must be positive.");
        }

        if (config.maxFrameDelayMs > config.minFrameDelayMs)
        {
            throw std::runtime_error("max_frame_delay_ms must be less than or equal to min_frame_delay_ms.");
        }

        if (config.treatsForMaxSpeed <= 0)
        {
            throw std::runtime_error("treats_for_max_speed must be greater than 0.");
        }

        return config;
    }

    class TerminalController
    {
    public:
        TerminalController()
        {
            if (tcgetattr(STDIN_FILENO, &originalState_) == -1)
            {
                throw std::runtime_error("Failed to read terminal state.");
            }

            termios rawState = originalState_;
            rawState.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
            rawState.c_cc[VMIN] = 0;
            rawState.c_cc[VTIME] = 0;

            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &rawState) == -1)
            {
                throw std::runtime_error("Failed to switch terminal to raw mode.");
            }

            std::cout << "\033[?25l";
            std::cout.flush();
        }

        TerminalController(const TerminalController&) = delete;
        TerminalController& operator=(const TerminalController&) = delete;

        ~TerminalController()
        {
            restore();
        }

        char pollInput() const
        {
            char ch = '\0';
            const ssize_t readResult = read(STDIN_FILENO, &ch, 1);
            if (readResult == 1)
            {
                return ch;
            }

            return '\0';
        }

        void restore()
        {
            if (!restored_)
            {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalState_);
                std::cout << "\033[?25h\033[0m";
                std::cout.flush();
                restored_ = true;
            }
        }

    private:
        termios originalState_ {};
        bool restored_ {false};
    };

    class Snake
    {
    public:
        Snake(int boardWidth, int boardHeight)
        {
            const int startX = boardWidth / 2;
            const int startY = boardHeight / 2;
            body_.push_back({startX, startY});
            body_.push_back({startX - 1, startY});
            body_.push_back({startX - 2, startY});
        }

        const std::deque<Position>& body() const
        {
            return body_;
        }

        Direction direction() const
        {
            return direction_;
        }

        void setDirection(Direction nextDirection)
        {
            if (isOpposite(nextDirection))
            {
                return;
            }

            direction_ = nextDirection;
        }

        Position nextHeadPosition() const
        {
            Position head = body_.front();

            switch (direction_)
            {
                case Direction::Up:
                    --head.y;
                    break;
                case Direction::Down:
                    ++head.y;
                    break;
                case Direction::Left:
                    --head.x;
                    break;
                case Direction::Right:
                    ++head.x;
                    break;
            }

            return head;
        }

        void advance(bool grow)
        {
            body_.push_front(nextHeadPosition());
            if (!grow)
            {
                body_.pop_back();
            }
        }

        bool occupies(const Position& position) const
        {
            for (const Position& segment : body_)
            {
                if (segment == position)
                {
                    return true;
                }
            }

            return false;
        }

        bool hitsItself() const
        {
            const Position& head = body_.front();
            for (std::size_t index = 1; index < body_.size(); ++index)
            {
                if (body_[index] == head)
                {
                    return true;
                }
            }

            return false;
        }

        std::size_t length() const
        {
            return body_.size();
        }

    private:
        bool isOpposite(Direction nextDirection) const
        {
            return (direction_ == Direction::Up && nextDirection == Direction::Down)
                || (direction_ == Direction::Down && nextDirection == Direction::Up)
                || (direction_ == Direction::Left && nextDirection == Direction::Right)
                || (direction_ == Direction::Right && nextDirection == Direction::Left);
        }

        std::deque<Position> body_ {};
        Direction direction_ {Direction::Right};
    };

    class SnakeGame
    {
    public:
        explicit SnakeGame(const GameConfig& config)
            : width_(config.boardWidth),
              height_(config.boardHeight),
              config_(config),
              snake_(config.boardWidth, config.boardHeight),
              randomEngine_(std::random_device {}())
        {
            spawnApple();
        }

        void run()
        {
            render();

            while (running_)
            {
                handleInput();
                update();
                render();
                std::this_thread::sleep_for(currentFrameDelay());
            }

            renderGameOver();
        }

        static void requestStop(int)
        {
            interrupted_ = true;
        }

    private:
        void handleInput()
        {
            const char input = terminal_.pollInput();
            switch (input)
            {
                case 'w':
                case 'W':
                    snake_.setDirection(Direction::Up);
                    break;
                case 's':
                case 'S':
                    snake_.setDirection(Direction::Down);
                    break;
                case 'a':
                case 'A':
                    snake_.setDirection(Direction::Left);
                    break;
                case 'd':
                case 'D':
                    snake_.setDirection(Direction::Right);
                    break;
                case 'q':
                case 'Q':
                    quitRequested_ = true;
                    running_ = false;
                    break;
                default:
                    break;
            }
        }

        void update()
        {
            if (!running_)
            {
                return;
            }

            if (interrupted_)
            {
                running_ = false;
                return;
            }

            const Position nextHead = snake_.nextHeadPosition();
            if (hitsWall(nextHead))
            {
                running_ = false;
                return;
            }

            const bool ateApple = nextHead == apple_;
            snake_.advance(ateApple);

            if (snake_.hitsItself())
            {
                running_ = false;
                return;
            }

            if (ateApple)
            {
                ++score_;
                if (snake_.length() == static_cast<std::size_t>((width_ - 2) * (height_ - 2)))
                {
                    won_ = true;
                    running_ = false;
                    return;
                }

                spawnApple();
            }
        }

        bool hitsWall(const Position& position) const
        {
            return position.x <= 0 || position.x >= width_ - 1
                || position.y <= 0 || position.y >= height_ - 1;
        }

        void spawnApple()
        {
            std::uniform_int_distribution<int> xDistribution(1, width_ - 2);
            std::uniform_int_distribution<int> yDistribution(1, height_ - 2);

            do
            {
                apple_ = {xDistribution(randomEngine_), yDistribution(randomEngine_)};
            }
            while (snake_.occupies(apple_));
        }

        void render() const
        {
            std::cout << "\033[2J\033[H";
            std::cout
                << "Snake  |  Score: " << score_
                << "  |  Delay: " << currentFrameDelay().count() << "ms"
                << "  |  Move: WASD  |  Quit: Q\n";

            for (int y = 0; y < height_; ++y)
            {
                for (int x = 0; x < width_; ++x)
                {
                    const Position current {x, y};

                    if (x == 0 || x == width_ - 1 || y == 0 || y == height_ - 1)
                    {
                        std::cout << '#';
                    }
                    else if (current == apple_)
                    {
                        std::cout << 'X';
                    }
                    else if (snake_.occupies(current))
                    {
                        std::cout << 'O';
                    }
                    else
                    {
                        std::cout << ' ';
                    }
                }

                std::cout << '\n';
            }

            std::cout.flush();
        }

        void renderGameOver()
        {
            terminal_.restore();

            if (won_)
            {
                std::cout << "\nYou filled the board. Final score: " << score_ << '\n';
            }
            else if (quitRequested_)
            {
                std::cout << "\nThanks for playing. Final score: " << score_ << '\n';
            }
            else
            {
                std::cout << "\nGame over. Final score: " << score_ << '\n';
            }
        }

        std::chrono::milliseconds currentFrameDelay() const
        {
            const int clampedTreats = score_ < config_.treatsForMaxSpeed ? score_ : config_.treatsForMaxSpeed;
            const double progress = static_cast<double>(clampedTreats) / static_cast<double>(config_.treatsForMaxSpeed);
            const double delayRange = static_cast<double>(config_.minFrameDelayMs - config_.maxFrameDelayMs);
            const int delay = static_cast<int>(config_.minFrameDelayMs - (delayRange * progress));
            return std::chrono::milliseconds(delay);
        }

        const int width_;
        const int height_;
        const GameConfig config_;
        Snake snake_;
        Position apple_ {};
        TerminalController terminal_ {};
        int score_ {0};
        bool running_ {true};
        bool won_ {false};
        bool quitRequested_ {false};
        std::mt19937 randomEngine_;
        static inline volatile std::sig_atomic_t interrupted_ {0};
    };
}

int main()
{
    try
    {
        std::signal(SIGINT, SnakeGame::requestStop);
        const std::filesystem::path configPath = executableDirectory() / "snake.conf";
        const GameConfig config = loadConfig(configPath.string());
        SnakeGame game(config);
        game.run();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
