class App
{
public:
    App() {}
    void run() {}
};

extern "C" void app_main()
{
    App app;
    app.run();
}