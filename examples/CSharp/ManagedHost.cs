namespace Example;

public sealed class MessageSource : IMessageSource
{
    public string Message => "C# project references work.";

    public static unsafe int Read(int* value) => *value;
}
