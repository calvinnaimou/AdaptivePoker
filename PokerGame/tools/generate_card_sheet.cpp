// One-time tool: generates a placeholder 13x4 card sprite sheet as a single
// PNG. Grid layout matches Card's enums directly - column = rank - 2 (TWO=0
// .. ACE=12), row = suit (SPADES=0, CLUBS=1, DIAMONDS=2, HEARTS=3) - so the
// game code can look up a cell with no extra translation table.
#include <SFML/Graphics.hpp>

#include <array>
#include <string>

namespace {
    //rendered at 3x the original 100x140 cells so cards stay crisp once
    //scaled down to their in-game display size (see CARD_SHEET_CELL_WIDTH/
    //HEIGHT in PokerGame/src/main.cpp, which must match this exactly)
    constexpr unsigned int CARD_WIDTH = 300;
    constexpr unsigned int CARD_HEIGHT = 420;
    constexpr unsigned int NUM_RANKS = 13;
    constexpr unsigned int NUM_SUITS = 4;

    const std::array<std::string, NUM_RANKS> RANK_LABELS = {
        "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"
    };

    //row order: SPADES, CLUBS, DIAMONDS, HEARTS - matches Suit's enum order
    constexpr std::array<char32_t, NUM_SUITS> SUIT_SYMBOLS = {
        0x2660, 0x2663, 0x2666, 0x2665
    };

    const std::array<sf::Color, NUM_SUITS> SUIT_COLORS = {
        sf::Color::Black, sf::Color::Black, sf::Color::Red, sf::Color::Red
    };
}

int main() {
    //anti-aliasing smooths the card outline/corners and the text glyphs -
    //without it the source art itself is jagged no matter how high its
    //resolution is
    sf::ContextSettings settings;
    settings.antiAliasingLevel = 4;

    sf::RenderTexture sheet;
    if (!sheet.resize({CARD_WIDTH * NUM_RANKS, CARD_HEIGHT * NUM_SUITS}, settings)) {
        return 1;
    }
    sheet.clear(sf::Color::Transparent);

    sf::Font font;
    if (!font.openFromFile("/System/Library/Fonts/Supplemental/Arial Unicode.ttf")) {
        return 1;
    }

    for (unsigned int row = 0; row < NUM_SUITS; ++row) {
        for (unsigned int col = 0; col < NUM_RANKS; ++col) {
            float x = static_cast<float>(col * CARD_WIDTH);
            float y = static_cast<float>(row * CARD_HEIGHT);
            sf::Color suitColor = SUIT_COLORS[row];

            sf::RectangleShape cardFace({CARD_WIDTH - 12.f, CARD_HEIGHT - 12.f});
            cardFace.setPosition({x + 6.f, y + 6.f});
            cardFace.setFillColor(sf::Color::White);
            cardFace.setOutlineColor(sf::Color::Black);
            cardFace.setOutlineThickness(6.f);
            sheet.draw(cardFace);

            sf::Text rankText(font, RANK_LABELS[col], 72);
            rankText.setFillColor(suitColor);
            rankText.setPosition({x + 24.f, y + 12.f});
            sheet.draw(rankText);

            sf::Text suitText(font, sf::String(SUIT_SYMBOLS[row]), 144);
            suitText.setFillColor(suitColor);
            suitText.setOrigin(suitText.getLocalBounds().getCenter());
            suitText.setPosition({x + CARD_WIDTH / 2.f, y + CARD_HEIGHT / 2.f + 30.f});
            sheet.draw(suitText);
        }
    }

    sheet.display();

    sf::Image image = sheet.getTexture().copyToImage();
    if (!image.saveToFile("assets/cards/deck.png")) {
        return 1;
    }

    return 0;
}
